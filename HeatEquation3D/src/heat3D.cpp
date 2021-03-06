/// this program solves the 3D heat equation on a 3D structured, cartesian grid using MPI.
///该程序使用MPI求解3D结构化笛卡尔网格上的3D热方程。

/**
 * The equation we want to solve can be expressed in the following way:
 我们要求解的方程式可以用以下方式表示：
 *
 * T_t = Dx * T_xx + Dy * T_yy + Dz * T_zz,
 *
 *
 * We use a second order accurate central scheme for the space derivatives, i.e. we have (in 1D):

在这里我们使用了下标符号，其中T_xx表示T在x方向上的二阶偏导数，并且类似地表示T_yy和T_zz。 Dx，Dy和Dz是热扩散强度，
这些强度决定了热量在每个方向上传播的速度。 我们在这里将各向同性（在空气和金属中很常见）的各向同性组装起来。
T是解向量，代表我们要求解的温度。

我们对空间导数使用二阶精确中心方案，即我们拥有（在一维中）

 * d^2 T(x) / dx^2 = T_xx ~= (T[i+1] - 2*T[i] + T[i-1]) / (dx^2)
 *
 * which we can apply in each coordinate direction equivalently. dx is the spacing between to adjacent cells, i.e. the
 * distance from one cell to its neighbors. It can be different for the y and z direction, however, within the same
 * direction it is always constant. For the time derivative, we use a first order Euler time integration scheme like so:

 我们可以将其等效地应用于每个坐标方向。 dx是相邻单元之间的间隔，即从一个单元到其相邻单元的距离。 y和z方向可以不同，
 但是在同一方向上，它始终是恒定的。 对于时间导数，我们使用一阶欧拉时间积分方案，如下所示：
 *
 * dT(x) / dt = T_t ~= (T[n+1] - T[n]) / dt
 *
 * Here, n is the timestep from the previous solution and n+1 is the timestep for the next solution. In this way we can
 * integrate our solution in time. Combining the two above approximations, we could write (dor a 1D equation)

 在此，n是上一个解决方案的时间步长，n + 1是下一个解决方案的时间步长。 这样，我们可以及时集成我们的解决方案。 结合以上两个近似值，我们可以写出（一维方程）
 *
 * T_t = Dx * T_xx =>
 * (T[n+1] - T[n]) / dt = Dx * (T[i+1] - 2*T[i] + T[i-1]) / (dx^2)
 *
 * We can solve this for T[n+1] to yield:

 * 我们可以解决这个问题，使T [n + 1]产生：
 * T[n+1] = T[n] + (dt * Dx / (dx^2)) * (T[i+1] - 2*T[i] + T[i-1])
 *
 * We have the information of the right hand side available, thus we can calculate T[n+1] for each i.
 * For i=0 or i=iend we need to specify boundary conditions and for all T[n] we need to specify initial conditions.
 * With those information available, we can loop over time and calculate an updated solution until the solution between
 * two consequtive time steps does not change more than a user-defined convergence threshold.

 我们拥有可用的右侧信息，因此我们可以为每个i计算T [n + 1]。 对于i = 0或i = iend，我们需要指定边界条件，对于所有T [n]，我们都需要指定初始条件。
 有了这些可用的信息，我们就可以随时间循环并计算更新的解决方案，直到两个相应时间步长之间的解决方案变化不超过用户定义的收敛阈值为止。
 *
 * For more information on the heat equation, you may check the following link:
 * https://www.uni-muenster.de/imperia/md/content/physik_tp/lectures/ws2016-2017/num_methods_i/heat.pdf
 */

#include <iostream>
#include <iomanip>
#include <algorithm>
#include <vector>
#include <array>
#include <fstream>
#include <limits>
#include <cmath>
#include <chrono>
#include <cassert>
#include "mpi.h"
#include<string>
#include <cuda.h>
#define DIM_THREAD_BLOCK_X 256
#define DIM_THREAD_BLOCK_Y 1
using namespace std;







// based on compiler flag, use either floats or doubles for floating point operations
//根据编译器标志，对浮点运算使用浮点数或双精度数

using floatT = double;
/*********************************************************************************************
                                                   GPU kernel methods
**********************************************************************************************/

#define MPI_FLOAT_T MPI_DOUBLE


/// enum used to index over the respective coordinate direction
enum COORDINATE { X = 0, Y, Z };

/// enum used to access the respective direction on each local processor
///用于访问每个本地处理器上的相应方向的枚举
/**
 *  0: LEFT
 *  1: RIGHT
 *  2: BOTTOM
 *  3: TOP
 *  4: BACK
 *  5: FRONT
 */
enum DIRECTION { LEFT = 0, RIGHT, BOTTOM, TOP, BACK, FRONT };

/// the number of physical dimensions, here 3 as we have a 3D domain
///物理尺寸的数量，这里是3，因为我们有3D域
#define NUMBER_OF_DIMENSIONS 3

int main(int argc, char** argv)
{
    /// if USE_MPI is defined (see makefile), execute the following code


  /// default ranks and size (number of processors), will be rearranged by cartesian topology
  ///默认ranks和大小（处理器数量），将按笛卡尔拓扑重新排列

    int rankDefaultMPICOMM, sizeDefaultMPICOMM;

    /// status and requests for non-blocking communications, i.e. MPI_IAllreduce(...) and MPI_IRecv(...)
    ///状态和非阻塞通信的请求，即MPI_IAllreduce（...）和MPI_IRecv（...）

    MPI_Status  status[NUMBER_OF_DIMENSIONS * 2];
    MPI_Status  postStatus[NUMBER_OF_DIMENSIONS];
    MPI_Request request[NUMBER_OF_DIMENSIONS * 2];
    MPI_Request reduceRequest;

    /// buffers into which we write data that we want to send and receive using MPI
    ///我们将要使用MPI发送和接收的数据写入其中的缓冲区
    /**
     * sendbuffer will be received into receivebuffer\
     sendbuffer将被接收到receivebuffer \
     */
    std::array<std::vector<floatT>, NUMBER_OF_DIMENSIONS * 2> sendBuffer;
    std::array<std::vector<floatT>, NUMBER_OF_DIMENSIONS * 2> receiveBuffer;

    /// initialise MPI and get default ranks and size
    MPI_Init(NULL, NULL);
    MPI_Comm_rank(MPI_COMM_WORLD, &rankDefaultMPICOMM);
    MPI_Comm_size(MPI_COMM_WORLD, &sizeDefaultMPICOMM);

    /// new MPI communicator for cartesian topologies
    /// 用于笛卡尔拓扑的新MPI通信器
    MPI_Comm MPI_COMM_CART;

    /// new rank and size for cartesian topology
    int       rank, size;

    /// tag used later during MPI_Send(...)
    ///稍后在MPI_Send（...）中使用的标记
    int       tagSend[NUMBER_OF_DIMENSIONS * 2];

    /// tag used later during MPI_IRecv(...)
    int       tagReceive[NUMBER_OF_DIMENSIONS * 2];

    /// the dimensions are equivalent to how we want our domain to be partitioned.
    /// 维度等同于我们希望域划分的方式。

    int       dimension3D[NUMBER_OF_DIMENSIONS] = { 0, 0, 0 };

    /// the coordinate in the current cartesian topology for the sub processor
    ///子处理进程当前笛卡尔拓扑中的坐标

    int       coordinates3D[NUMBER_OF_DIMENSIONS];

    /// flags to indicate if we have period boundary conditions
    /// 表示我们是否有周期边界条件的标志
    const int periods3D[NUMBER_OF_DIMENSIONS] = { false, false, false };

    /// neighbors hold the rank of the neighboring processors and are accessed with the DIRECTION enum
    /// 邻居拥有相邻处理器的等级，并通过DIRECTION枚举进行访问

    int       neighbors[NUMBER_OF_DIMENSIONS * 2];

    /// MPI tries to find the best possible partition of our domain and stores that in dimension3D
    /// MPI尝试找到我们域的最佳分区并将其存储在Dimension3D中

    MPI_Dims_create(sizeDefaultMPICOMM, NUMBER_OF_DIMENSIONS, dimension3D);

    /// based on the partition, we create a new cartesian topology which simplifies communication
    /// 基于该分区，我们创建一个新的笛卡尔拓扑，从而简化了通信
    MPI_Cart_create(MPI_COMM_WORLD, NUMBER_OF_DIMENSIONS, dimension3D, periods3D, true, &MPI_COMM_CART);

    /// These calls will find the direct neighbors for each processors and return MPI_PROC_NULL if no neighbor is found.
    /// 这些调用将找到每个处理器的直接邻居，如果找不到邻居，则返回MPI_PROC_NULL。
    MPI_Cart_shift(MPI_COMM_CART, COORDINATE::X, 1, &neighbors[DIRECTION::LEFT], &neighbors[DIRECTION::RIGHT]);
    MPI_Cart_shift(MPI_COMM_CART, COORDINATE::Y, 1, &neighbors[DIRECTION::BOTTOM], &neighbors[DIRECTION::TOP]);
    MPI_Cart_shift(MPI_COMM_CART, COORDINATE::Z, 1, &neighbors[DIRECTION::BACK], &neighbors[DIRECTION::FRONT]);

    /// get the new rank and size for the cartesian topology
    /// 获取笛卡尔拓扑的新等级和大小

    MPI_Comm_rank(MPI_COMM_CART, &rank);
    MPI_Comm_size(MPI_COMM_CART, &size);

    /// get the coordinates inside our cartesian topology
    /// 获取我们的笛卡尔拓扑内的坐标
    MPI_Cart_coords(MPI_COMM_CART, rank, NUMBER_OF_DIMENSIONS, coordinates3D);

    /// if USE_SEQUENTIAL is defined (see makefile), execute the following code
      /// 如果定义了USE_SEQUENTIAL（请参见makefile），请执行以下代码



    /// check that we have the right number of input arguments
      /// 检查我们是否有正确数量的输入参数
    /**
     * this is the order in which we need to pass in the command line argument:

     这是我们需要在命令行参数中传递的顺序：
     *
     * argv[0]: name of compiled program
     * argv[1]: number of cells in the x direction
     * argv[2]: number of cells in the y direction
     * argv[3]: number of cells in the z direction
     * argv[4]: maximum number of iterations to be used by time loop    时间循环要使用的最大迭代次数
     * argv[5]: convergence criterion to be used to check if a solution has converged 收敛标准，用于检查解决方案是否收敛
     */
    if (rank == 0) {
        if (argc != 6) {
            std::cout << "Incorrect number of command line arguments specified, use the following syntax:\n" << std::endl;
            std::cout << "bin/HeatEquation3D NUM_CELLS_X NUM_CELLS_Y NUM_CELLS_Z ITER_MAX EPS" << std::endl;
            std::cout << "\nor, using MPI, use the following syntax:\n" << std::endl;
            std::cout << "mpirun -n NUM_PROCS bin/HeatEquation3D NUM_CELLS_X NUM_CELLS_Y NUM_CELLS_Z ITER_MAX EPS" << std::endl;
            std::cout << "\nSee source code for additional informations!" << std::endl;
            std::abort();
        }
        else {
            std::cout << "Runnung HeatEquation3D with the following arguments: " << std::endl;
            std::cout << "executable:               " << argv[0] << std::endl;
            std::cout << "number of cells in x:     " << std::stoi(argv[1]) << std::endl;
            std::cout << "number of cells in y:     " << std::stoi(argv[2]) << std::endl;
            std::cout << "number of cells in z:     " << std::stoi(argv[3]) << std::endl;
            std::cout << "max number of iterations: " << std::stoi(argv[4]) << std::endl;


            std::cout << "convergence threshold:    " << std::stod(argv[5]) << "\n" << std::endl;

        }
    }

    /// maximum number of iterations to perform in time loop
    /// 在时间循环中要执行的最大迭代次数

    const unsigned iterMax = std::stoi(argv[4]);

    /// convergence criterion, which, once met, will terminate the calculation
    /// 收敛准则，一旦满足，将终止计算



    const floatT eps = std::stod(argv[5]);


    /// both variables are used to calculate the convergence and normalise the result.
      /// 这两个变量都用于计算收敛性和标准化结果。
    /**
     * We have two normalisation factors as we have to perform a reduction first (if we use MPI) to have a globally
     * available normalisation factor

     我们有两个归一化因子，因为我们必须首先执行归约（如果使用MPI）才能拥有全局可用的归一化因子
     */
    floatT globalNorm = 1.0;
    floatT norm = 1.0;

    /// the break conditions used for checking of convergence has been achieved and the simulation should be stopped.
    /// 已经达到用于收敛性检查的中断条件，应该停止模拟。
    int breakCondition = false;
    int globalBreakCondition = false;

    /// number of points (in total, not per processor) in x, y and z.
    /// x，y和z中的点数（总计，不是每个处理器）。
    unsigned numCells[NUMBER_OF_DIMENSIONS];
    numCells[COORDINATE::X] = std::stoi(argv[1]);
    numCells[COORDINATE::Y] = std::stoi(argv[2]);
    numCells[COORDINATE::Z] = std::stoi(argv[3]);

    /// length of the domain in x, y and z.
    /// 域的长度，以x，y和z表示。
    floatT domainLength[NUMBER_OF_DIMENSIONS];
    domainLength[COORDINATE::X] = 1.0;
    domainLength[COORDINATE::Y] = 1.0;
    domainLength[COORDINATE::Z] = 1.0;

    /// thermal conductivity parameter. 导热系数。

    const floatT alpha = 1.0;

    /// The courant fridrich levy number                    Courant Fridrich征费编号

    const floatT CFL = 0.4;

    /// the distance between cells in the x, y and z direction.
    /// x，y和z方向上像元之间的距离。
    floatT spacing[NUMBER_OF_DIMENSIONS];
    spacing[COORDINATE::X] = domainLength[COORDINATE::X] / static_cast<floatT>(numCells[COORDINATE::X] - 1.0);
    spacing[COORDINATE::Y] = domainLength[COORDINATE::Y] / static_cast<floatT>(numCells[COORDINATE::Y] - 1.0);
    spacing[COORDINATE::Z] = domainLength[COORDINATE::Z] / static_cast<floatT>(numCells[COORDINATE::Z] - 1.0);

    /// the timestep to be used in the time integration.
    /// 时间积分中要使用的时间步。
    const floatT dt = CFL * 1.0 / (NUMBER_OF_DIMENSIONS * 2) *
        std::pow(std::min({ spacing[COORDINATE::X], spacing[COORDINATE::Y], spacing[COORDINATE::Z] }), 2.0) / alpha;

    /// thermal diffusivity strength in the x, y and z direction.
    /// x，y和z方向的热扩散强度。

    const floatT Dx = dt * alpha / (std::pow(spacing[COORDINATE::X], 2.0));
    const floatT Dy = dt * alpha / (std::pow(spacing[COORDINATE::Y], 2.0));
    const floatT Dz = dt * alpha / (std::pow(spacing[COORDINATE::Z], 2.0));

    /// numer of iterations taken to converge solution. will be set once simulation has converged.
    /// 收敛求解的迭代次数。 仿真收敛后将设置
    unsigned finalNumIterations = 0;


    /// assure that the partition given to use by MPI can be used to partition our domain in each direction
  /// 确保由MPI使用的分区可用于在每个方向上对我们的域进行分区
    assert((numCells[COORDINATE::X] - 1) % dimension3D[COORDINATE::X] == 0 &&
        "Can not partition data for given number of processors in x!");
    assert((numCells[COORDINATE::Y] - 1) % dimension3D[COORDINATE::Y] == 0 &&
        "Can not partition data for given number of processors in y!");
    assert((numCells[COORDINATE::Z] - 1) % dimension3D[COORDINATE::Z] == 0 &&
        "Can not partition data for given number of processors in z!");

    /// chunck contains the number of cells in the x, y and z direction for each sub domain.
    /// 块包含每个子域在xy和z方向上的单元数。

    const unsigned chunck[NUMBER_OF_DIMENSIONS] = {
      ((numCells[COORDINATE::X] - 1) / dimension3D[COORDINATE::X]) + 1,
      ((numCells[COORDINATE::Y] - 1) / dimension3D[COORDINATE::Y]) + 1,
      ((numCells[COORDINATE::Z] - 1) / dimension3D[COORDINATE::Z]) + 1
    };


    /// Create a solution vector

    std::vector<std::vector<std::vector<floatT>>> T, T0;

    /// resize both T and T0 for each sub-domain
    /// 调整每个子域的T和T0的大小
    T.resize(chunck[COORDINATE::X]);
    T0.resize(chunck[COORDINATE::X]);
    for (unsigned i = 0; i < chunck[COORDINATE::X]; ++i) {
        T[i].resize(chunck[COORDINATE::Y]);
        T0[i].resize(chunck[COORDINATE::Y]);
        for (unsigned j = 0; j < chunck[COORDINATE::Y]; ++j) {
            T[i][j].resize(chunck[COORDINATE::Z]);
            T0[i][j].resize(chunck[COORDINATE::Z]);
        }
    }

    /// initialise each solution vector on each sub-domain with zero everywhere
    /// 初始化每个子域上的每个解向量，各处零
    for (unsigned i = 0; i < chunck[COORDINATE::X]; ++i)
        for (unsigned j = 0; j < chunck[COORDINATE::Y]; ++j)
            for (unsigned k = 0; k < chunck[COORDINATE::Z]; ++k)
                T[i][j][k] = 0.0;

    /// apply boundary conditions on the top of the domain
    /// 在域顶部应用边界条件

    if (neighbors[DIRECTION::TOP] == MPI_PROC_NULL)

        for (unsigned i = 0; i < chunck[COORDINATE::X]; ++i)
            for (unsigned k = 0; k < chunck[COORDINATE::Z]; ++k)
                T[i][chunck[COORDINATE::Y] - 1][k] = 1.0;

    /// apply boundary conditions on the left-side of the domain
      /// 在域的左侧应用边界条件

    if (neighbors[DIRECTION::LEFT] == MPI_PROC_NULL)

        for (unsigned j = 0; j < chunck[COORDINATE::Y]; ++j)
            for (unsigned k = 0; k < chunck[COORDINATE::Z]; ++k)
                T[0][j][k] = (coordinates3D[COORDINATE::Y] * (chunck[COORDINATE::Y] - 1) + j) * spacing[COORDINATE::Y];

    /// apply boundary conditions on the right-side of the domain
      /// 在域的右侧应用边界条件

    if (neighbors[DIRECTION::RIGHT] == MPI_PROC_NULL)

        for (unsigned j = 0; j < chunck[COORDINATE::Y]; ++j)
            for (unsigned k = 0; k < chunck[COORDINATE::Z]; ++k)
                T[chunck[COORDINATE::X] - 1][j][k] = (coordinates3D[COORDINATE::Y] * (chunck[COORDINATE::Y] - 1) + j) * spacing[COORDINATE::Y];

    /// apply boundary conditions on the back-side of the domain
      /// 在域的背面应用边界条件

    if (neighbors[DIRECTION::BACK] == MPI_PROC_NULL)

        for (unsigned i = 0; i < chunck[COORDINATE::X]; ++i)
            for (unsigned j = 0; j < chunck[COORDINATE::Y]; ++j)
                T[i][j][0] = (coordinates3D[COORDINATE::Y] * (chunck[COORDINATE::Y] - 1) + j) * spacing[COORDINATE::Y];

    /// apply boundary conditions on the front-side of the domain
      /// 在域的前端应用边界条件

    if (neighbors[DIRECTION::FRONT] == MPI_PROC_NULL)

        for (unsigned i = 0; i < chunck[COORDINATE::X]; ++i)
            for (unsigned j = 0; j < chunck[COORDINATE::Y]; ++j)
                T[i][j][chunck[COORDINATE::Z] - 1] = (coordinates3D[COORDINATE::Y] * (chunck[COORDINATE::Y] - 1) + j) * spacing[COORDINATE::Y];

    /// if we use MPI, make sure that our send and recieve buffers are correctly allocated
      /// 如果我们使用MPI，请确保正确分配了您的发送和接收缓冲区


  /// allocate storage for left-side send- and recievebuffer
  /// 为左侧的发送和接收缓冲区分配存储空间

    if (neighbors[DIRECTION::LEFT] != MPI_PROC_NULL) {
        sendBuffer[DIRECTION::LEFT].resize((chunck[COORDINATE::Y] - 1) * (chunck[COORDINATE::Z] - 1));
        receiveBuffer[DIRECTION::LEFT].resize((chunck[COORDINATE::Y] - 1) * (chunck[COORDINATE::Z] - 1));
    }
    else {
        sendBuffer[DIRECTION::LEFT].resize(1);
        receiveBuffer[DIRECTION::LEFT].resize(1);
    }

    /// allocate storage for right-side send- and recievebuffer
    /// 为右侧的发送和接收缓冲区分配存储空间

    if (neighbors[DIRECTION::RIGHT] != MPI_PROC_NULL) {
        sendBuffer[DIRECTION::RIGHT].resize((chunck[COORDINATE::Y] - 1) * (chunck[COORDINATE::Z] - 1));
        receiveBuffer[DIRECTION::RIGHT].resize((chunck[COORDINATE::Y] - 1) * (chunck[COORDINATE::Z] - 1));
    }
    else {
        sendBuffer[DIRECTION::RIGHT].resize(1);
        receiveBuffer[DIRECTION::RIGHT].resize(1);
    }

    /// allocate storage for bottom-side send- and recievebuffer
    /// 为底部发送和接收缓冲区分配存储

    if (neighbors[DIRECTION::BOTTOM] != MPI_PROC_NULL) {
        sendBuffer[DIRECTION::BOTTOM].resize((chunck[COORDINATE::X] - 1) * (chunck[COORDINATE::Z] - 1));
        receiveBuffer[DIRECTION::BOTTOM].resize((chunck[COORDINATE::X] - 1) * (chunck[COORDINATE::Z] - 1));
    }
    else {
        sendBuffer[DIRECTION::BOTTOM].resize(1);
        receiveBuffer[DIRECTION::BOTTOM].resize(1);
    }

    /// allocate storage for top-side send- and recievebuffer
    /// 为顶部发送和接收缓冲区分配存储

    if (neighbors[DIRECTION::TOP] != MPI_PROC_NULL) {
        sendBuffer[DIRECTION::TOP].resize((chunck[COORDINATE::X] - 1) * (chunck[COORDINATE::Z] - 1));
        receiveBuffer[DIRECTION::TOP].resize((chunck[COORDINATE::X] - 1) * (chunck[COORDINATE::Z] - 1));
    }
    else {
        sendBuffer[DIRECTION::TOP].resize(1);
        receiveBuffer[DIRECTION::TOP].resize(1);

    }

    /// allocate storage for back-side send- and recievebuffer
    /// 为后端发送和接收缓冲区分配存储

    if (neighbors[DIRECTION::BACK] != MPI_PROC_NULL) {
        sendBuffer[DIRECTION::BACK].resize((chunck[COORDINATE::X] - 1) * (chunck[COORDINATE::Y] - 1));
        receiveBuffer[DIRECTION::BACK].resize((chunck[COORDINATE::X] - 1) * (chunck[COORDINATE::Y] - 1));
    }
    else {
        sendBuffer[DIRECTION::BACK].resize(1);
        receiveBuffer[DIRECTION::BACK].resize(1);
    }

    /// allocate storage for front-side send- and recievebuffer
    /// 为前端发送和接收缓冲区分配存储

    if (neighbors[DIRECTION::FRONT] != MPI_PROC_NULL) {
        sendBuffer[DIRECTION::FRONT].resize((chunck[COORDINATE::X] - 1) * (chunck[COORDINATE::Y] - 1));
        receiveBuffer[DIRECTION::FRONT].resize((chunck[COORDINATE::X] - 1) * (chunck[COORDINATE::Y] - 1));
    }
    else {
        sendBuffer[DIRECTION::FRONT].resize(1);
        receiveBuffer[DIRECTION::FRONT].resize(1);
    }

    /// start timing (we don't want any setup time to be included, thus we start it just before the time loop)
    /// 开始计时（我们不希望包含任何设置时间，因此我们在时间循环之前开始计时）
    auto start = MPI_Wtime();


    /// main time loop
    /**
     * this is where we solve the actual partial differential equation and do the communication among processors.
     这是我们求解实际偏微分方程并进行处理器之间通信的地方。
     */



   



    for (unsigned time = 0; time < iterMax; ++time)
    {
        /// copy the solution from the previous timestep into T, which holds the solution of the last iteration
          /// 将前一个时间步的解决方案复制到T，其中保存最后一次迭代的解决方案

        for (unsigned i = 0; i < chunck[COORDINATE::X]; ++i)
            for (unsigned j = 0; j < chunck[COORDINATE::Y]; ++j)
                for (unsigned k = 0; k < chunck[COORDINATE::Z]; ++k)
                    T0[i][j][k] = T[i][j][k];

        // HALO communication step



  /// preparing the send buffer (the data we want to send to the left neighbor), if a neighbor exists
///如果存在邻居，则准备发送缓冲区（我们要发送到左邻居的数据）

  /**
   * for simplicity, we write the 2D array (the face on the boundary) into a 1D array which we can easily send.
   * It is important that once we receive the it we are aware that the array containing the data is 1D now.

   为简单起见，我们将2D数组（边界上的面）写入一个1D数组中，我们可以轻松地发送它。 重要的是，一旦收到它，
   我们就知道包含数据的数组现在是一维的。
   */
        unsigned counter = 0;
        if (neighbors[DIRECTION::LEFT] != MPI_PROC_NULL)
            for (unsigned j = 1; j < chunck[COORDINATE::Y] - 1; ++j)
                for (unsigned k = 1; k < chunck[COORDINATE::Z] - 1; ++k)
                    sendBuffer[DIRECTION::LEFT][counter++] = T0[1][j][k];

        /// preparing the send buffer (the data we want to send to the right neighbor), if a neighbor exists
        /// 如果存在邻居，则准备发送缓冲区（我们要发送到正确邻居的数据）
        counter = 0;
        if (neighbors[DIRECTION::RIGHT] != MPI_PROC_NULL)
            for (unsigned j = 1; j < chunck[COORDINATE::Y] - 1; ++j)
                for (unsigned k = 1; k < chunck[COORDINATE::Z] - 1; ++k)
                    sendBuffer[DIRECTION::RIGHT][counter++] = T0[chunck[COORDINATE::X] - 2][j][k];

        /// preparing the send buffer (the data we want to send to the bottom neighbor), if a neighbor exists
        /// 如果存在邻居，则准备发送缓冲区（我们要发送到最底层邻居的数据）
        counter = 0;
        if (neighbors[DIRECTION::BOTTOM] != MPI_PROC_NULL)
            for (unsigned i = 1; i < chunck[COORDINATE::X] - 1; ++i)
                for (unsigned k = 1; k < chunck[COORDINATE::Z] - 1; ++k)
                    sendBuffer[DIRECTION::BOTTOM][counter++] = T0[i][1][k];

        /// preparing the send buffer (the data we want to send to the top neighbor), if a neighbor exists
        /// 如果存在邻居，则准备发送缓冲区（我们要发送到最上面的邻居的数据）

        counter = 0;
        if (neighbors[DIRECTION::TOP] != MPI_PROC_NULL)
            for (unsigned i = 1; i < chunck[COORDINATE::X] - 1; ++i)
                for (unsigned k = 1; k < chunck[COORDINATE::Z] - 1; ++k)
                    sendBuffer[DIRECTION::TOP][counter++] = T0[i][chunck[COORDINATE::Y] - 2][k];

        /// preparing the send buffer (the data we want to send to the back neighbor), if a neighbor exists
        /// 如果存在邻居，则准备发送缓冲区（我们要发送给后邻居的数据）
        counter = 0;
        if (neighbors[DIRECTION::BACK] != MPI_PROC_NULL)
            for (unsigned i = 1; i < chunck[COORDINATE::X] - 1; ++i)
                for (unsigned j = 1; j < chunck[COORDINATE::Y] - 1; ++j)
                    sendBuffer[DIRECTION::BACK][counter++] = T0[i][j][1];

        /// preparing the send buffer (the data we want to send to the front neighbor), if a neighbor exists
        /// 如果存在邻居，则准备发送缓冲区（我们要发送到前邻居的数据）
        counter = 0;
        if (neighbors[DIRECTION::FRONT] != MPI_PROC_NULL)
            for (unsigned i = 1; i < chunck[COORDINATE::X] - 1; ++i)
                for (unsigned j = 1; j < chunck[COORDINATE::Y] - 1; ++j)
                    sendBuffer[DIRECTION::FRONT][counter++] = T0[i][j][chunck[COORDINATE::Z] - 2];


       

        /// prepare the tags we need to append to the send message for each send (in each direction) and receive
        /// 准备我们需要为每个发送（在每个方向）附加到发送消息的标签，并接收

        for (unsigned index = 0; index < NUMBER_OF_DIMENSIONS * 2; ++index) {
            tagSend[index] = 100 + neighbors[index];
            tagReceive[index] = 100 + rank;
        }

        /// send the prepared send buffer to the neighbors using non-blocking MPI_Isend(...)
        /// 使用非阻塞MPI_Isend（...）将准备好的发送缓冲区发送给邻居
        MPI_Isend(&sendBuffer[DIRECTION::LEFT][0], (chunck[COORDINATE::Y] - 1) * (chunck[COORDINATE::Z] - 1),
            MPI_FLOAT_T, neighbors[DIRECTION::LEFT], tagSend[DIRECTION::LEFT], MPI_COMM_CART,
            &request[DIRECTION::LEFT]);

        MPI_Isend(&sendBuffer[DIRECTION::RIGHT][0], (chunck[COORDINATE::Y] - 1) * (chunck[COORDINATE::Z] - 1),
            MPI_FLOAT_T, neighbors[DIRECTION::RIGHT], tagSend[DIRECTION::RIGHT], MPI_COMM_CART,
            &request[DIRECTION::RIGHT]);

        MPI_Isend(&sendBuffer[DIRECTION::BOTTOM][0], (chunck[COORDINATE::X] - 1) * (chunck[COORDINATE::Z] - 1),
            MPI_FLOAT_T, neighbors[DIRECTION::BOTTOM], tagSend[DIRECTION::BOTTOM], MPI_COMM_CART,
            &request[DIRECTION::BOTTOM]);

        MPI_Isend(&sendBuffer[DIRECTION::TOP][0], (chunck[COORDINATE::X] - 1) * (chunck[COORDINATE::Z] - 1),
            MPI_FLOAT_T, neighbors[DIRECTION::TOP], tagSend[DIRECTION::TOP], MPI_COMM_CART,
            &request[DIRECTION::TOP]);

        MPI_Isend(&sendBuffer[DIRECTION::BACK][0], (chunck[COORDINATE::X] - 1) * (chunck[COORDINATE::Y] - 1),
            MPI_FLOAT_T, neighbors[DIRECTION::BACK], tagSend[DIRECTION::BACK], MPI_COMM_CART,
            &request[DIRECTION::BACK]);

        MPI_Isend(&sendBuffer[DIRECTION::FRONT][0], (chunck[COORDINATE::X] - 1) * (chunck[COORDINATE::Y] - 1),
            MPI_FLOAT_T, neighbors[DIRECTION::FRONT], tagSend[DIRECTION::FRONT], MPI_COMM_CART,
            &request[DIRECTION::FRONT]);


        /*****************************************************************************************************************
                                                          GPU BEGIN
   ****************************************************************************************************************/
        // compute internal domain (no halos required)
          // 计算内部域（无需光晕）
        
        

        /// now work on the halo cells
        /// 现在可以在晕圈上工作

  /// receive the halo information from each neighbor, if exists.
/// 从每个邻居（如果存在）接收光晕信息。

        MPI_Recv(&receiveBuffer[DIRECTION::LEFT][0], (chunck[COORDINATE::Y] - 1) * (chunck[COORDINATE::Z] - 1),
            MPI_FLOAT_T, neighbors[DIRECTION::LEFT], tagReceive[DIRECTION::LEFT], MPI_COMM_CART,
            &status[DIRECTION::LEFT]);

        MPI_Recv(&receiveBuffer[DIRECTION::RIGHT][0], (chunck[COORDINATE::Y] - 1) * (chunck[COORDINATE::Z] - 1),
            MPI_FLOAT_T, neighbors[DIRECTION::RIGHT], tagReceive[DIRECTION::RIGHT], MPI_COMM_CART,
            &status[DIRECTION::RIGHT]);

        MPI_Recv(&receiveBuffer[DIRECTION::BOTTOM][0], (chunck[COORDINATE::X] - 1) * (chunck[COORDINATE::Z] - 1),
            MPI_FLOAT_T, neighbors[DIRECTION::BOTTOM], tagReceive[DIRECTION::BOTTOM], MPI_COMM_CART,
            &status[DIRECTION::BOTTOM]);

        MPI_Recv(&receiveBuffer[DIRECTION::TOP][0], (chunck[COORDINATE::X] - 1) * (chunck[COORDINATE::Z] - 1),
            MPI_FLOAT_T, neighbors[DIRECTION::TOP], tagReceive[DIRECTION::TOP], MPI_COMM_CART,
            &status[DIRECTION::TOP]);

        MPI_Recv(&receiveBuffer[DIRECTION::BACK][0], (chunck[COORDINATE::X] - 1) * (chunck[COORDINATE::Y] - 1),
            MPI_FLOAT_T, neighbors[DIRECTION::BACK], tagReceive[DIRECTION::BACK], MPI_COMM_CART,
            &status[DIRECTION::BACK]);

        MPI_Recv(&receiveBuffer[DIRECTION::FRONT][0], (chunck[COORDINATE::X] - 1) * (chunck[COORDINATE::Y] - 1),
            MPI_FLOAT_T, neighbors[DIRECTION::FRONT], tagReceive[DIRECTION::FRONT], MPI_COMM_CART,
            &status[DIRECTION::FRONT]);

        /// make sure that all communications have been executed
        /// 确保所有通信均已执行
        /**
         * even though we use a blocking receive here, since we used a non-blocking send, we have to wait for all
         * communications to have finished before continuing.
         即使我们在此处使用阻塞接收，但由于我们使用了非阻塞发送，因此我们必须等待所有通信完成后才能继续。
         */
        MPI_Waitall(NUMBER_OF_DIMENSIONS * 2, request, status);

        /// now that we have the halo cells, we update the boundaries using information from other processors
        /// 现在我们有了光晕单元，我们使用其他处理器的信息更新边界

        if (neighbors[DIRECTION::LEFT] != MPI_PROC_NULL) {
            const auto& THalo = receiveBuffer[DIRECTION::LEFT];
            unsigned i = 0;
            unsigned counter = 0;

            for (unsigned j = 1; j < chunck[COORDINATE::Y] - 1; ++j)
                for (unsigned k = 1; k < chunck[COORDINATE::Z] - 1; ++k) {
                    T[i][j][k] = T0[i][j][k] +
                        Dx * (T0[i + 1][j][k] - 2.0 * T0[i][j][k] + THalo[counter++]) +
                        Dy * (T0[i][j + 1][k] - 2.0 * T0[i][j][k] + T0[i][j - 1][k]) +
                        Dz * (T0[i][j][k + 1] - 2.0 * T0[i][j][k] + T0[i][j][k - 1]);
                }
        }

        /// do the same as above, this time for the right neighbor halo data
        /// 进行与上述相同的操作，这次用于正确的邻居光晕数据

        if (neighbors[DIRECTION::RIGHT] != MPI_PROC_NULL) {
            const auto& THalo = receiveBuffer[DIRECTION::RIGHT];
            unsigned i = chunck[COORDINATE::X] - 1;
            unsigned counter = 0;

            for (unsigned j = 1; j < chunck[COORDINATE::Y] - 1; ++j)
                for (unsigned k = 1; k < chunck[COORDINATE::Z] - 1; ++k) {
                    T[i][j][k] = T0[i][j][k] +
                        Dx * (THalo[counter++] - 2.0 * T0[i][j][k] + T0[i - 1][j][k]) +
                        Dy * (T0[i][j + 1][k] - 2.0 * T0[i][j][k] + T0[i][j - 1][k]) +
                        Dz * (T0[i][j][k + 1] - 2.0 * T0[i][j][k] + T0[i][j][k - 1]);
                }
        }

        /// do the same as above, this time for the bottom neighbor halo data
        /// 与上面相同，这一次是针对底部邻居的光晕数据
        if (neighbors[DIRECTION::BOTTOM] != MPI_PROC_NULL) {
            const auto& THalo = receiveBuffer[DIRECTION::BOTTOM];
            unsigned j = 0;
            unsigned counter = 0;

            for (unsigned i = 1; i < chunck[COORDINATE::X] - 1; ++i)
                for (unsigned k = 1; k < chunck[COORDINATE::Z] - 1; ++k) {
                    T[i][j][k] = T0[i][j][k] +
                        Dx * (T0[i + 1][j][k] - 2.0 * T0[i][j][k] + T0[i - 1][j][k]) +
                        Dy * (T0[i][j + 1][k] - 2.0 * T0[i][j][k] + THalo[counter++]) +
                        Dz * (T0[i][j][k + 1] - 2.0 * T0[i][j][k] + T0[i][j][k - 1]);
                }
        }

        /// do the same as above, this time for the top neighbor halo data
        /// 与上面相同，这一次是对最邻近的光晕数据
        if (neighbors[DIRECTION::TOP] != MPI_PROC_NULL) {
            const auto& THalo = receiveBuffer[DIRECTION::TOP];
            unsigned j = chunck[COORDINATE::Y] - 1;
            unsigned counter = 0;

            for (unsigned i = 1; i < chunck[COORDINATE::X] - 1; ++i)
                for (unsigned k = 1; k < chunck[COORDINATE::Z] - 1; ++k) {
                    T[i][j][k] = T0[i][j][k] +
                        Dx * (T0[i + 1][j][k] - 2.0 * T0[i][j][k] + T0[i - 1][j][k]) +
                        Dy * (THalo[counter++] - 2.0 * T0[i][j][k] + T0[i][j - 1][k]) +
                        Dz * (T0[i][j][k + 1] - 2.0 * T0[i][j][k] + T0[i][j][k - 1]);
                }
        }

        /// do the same as above, this time for the back neighbor halo data
        // 与上述相同，这一次用于后邻晕数据
        if (neighbors[DIRECTION::BACK] != MPI_PROC_NULL) {
            const auto& THalo = receiveBuffer[DIRECTION::BACK];
            unsigned k = 0;
            unsigned counter = 0;

            for (unsigned i = 1; i < chunck[COORDINATE::X] - 1; ++i)
                for (unsigned j = 1; j < chunck[COORDINATE::Y] - 1; ++j) {
                    T[i][j][k] = T0[i][j][k] +
                        Dx * (T0[i + 1][j][k] - 2.0 * T0[i][j][k] + T0[i - 1][j][k]) +
                        Dy * (T0[i][j + 1][k] - 2.0 * T0[i][j][k] + T0[i][j - 1][k]) +
                        Dz * (T0[i][j][k + 1] - 2.0 * T0[i][j][k] + THalo[counter++]);
                }
        }

        /// do the same as above, this time for the front neighbor halo data
        /// 与上面相同，这一次用于前邻光晕数据
        if (neighbors[DIRECTION::FRONT] != MPI_PROC_NULL) {
            const auto& THalo = receiveBuffer[DIRECTION::FRONT];
            unsigned k = chunck[COORDINATE::Z] - 1;
            unsigned counter = 0;

            for (unsigned i = 1; i < chunck[COORDINATE::X] - 1; ++i)
                for (unsigned j = 1; j < chunck[COORDINATE::Y] - 1; ++j) {
                    T[i][j][k] = T0[i][j][k] +
                        Dx * (T0[i + 1][j][k] - 2.0 * T0[i][j][k] + T0[i - 1][j][k]) +
                        Dy * (T0[i][j + 1][k] - 2.0 * T0[i][j][k] + T0[i][j - 1][k]) +
                        Dz * (THalo[counter++] - 2.0 * T0[i][j][k] + T0[i][j][k - 1]);
                }
        }
        /************************************************************************************************************

                                                                GPU      END

       ***********************************************************************************************************/
        /// update edges of halo elements
        /// 更新光晕元素的边缘
        if (neighbors[DIRECTION::LEFT] != MPI_PROC_NULL) {
            if (neighbors[DIRECTION::BOTTOM] != MPI_PROC_NULL) {
                unsigned i = 0;
                unsigned j = 0;
                for (unsigned k = 1; k < chunck[COORDINATE::Z] - 1; ++k)
                    T[i][j][k] = 2.0 * T[i + 1][j][k] - T[i + 2][j][k];
            }
            if (neighbors[DIRECTION::TOP] != MPI_PROC_NULL) {
                unsigned i = 0;
                unsigned j = chunck[COORDINATE::Y] - 1;
                for (unsigned k = 1; k < chunck[COORDINATE::Z] - 1; ++k)
                    T[i][j][k] = 2.0 * T[i + 1][j][k] - T[i + 2][j][k];
            }
            if (neighbors[DIRECTION::BACK] != MPI_PROC_NULL) {
                unsigned i = 0;
                unsigned k = 0;
                for (unsigned j = 1; j < chunck[COORDINATE::Y] - 1; ++j)
                    T[i][j][k] = 2.0 * T[i + 1][j][k] - T[i + 2][j][k];
            }
            if (neighbors[DIRECTION::FRONT] != MPI_PROC_NULL) {
                unsigned i = 0;
                unsigned k = chunck[COORDINATE::Z] - 1;
                for (unsigned j = 1; j < chunck[COORDINATE::Y] - 1; ++j)
                    T[i][j][k] = 2.0 * T[i + 1][j][k] - T[i + 2][j][k];
            }
        }

        if (neighbors[DIRECTION::RIGHT] != MPI_PROC_NULL) {
            if (neighbors[DIRECTION::BOTTOM] != MPI_PROC_NULL) {
                unsigned i = chunck[COORDINATE::X] - 1;
                unsigned j = 0;
                for (unsigned k = 1; k < chunck[COORDINATE::Z] - 1; ++k)
                    T[i][j][k] = 2.0 * T[i - 1][j][k] - T[i - 2][j][k];
            }
            if (neighbors[DIRECTION::TOP] != MPI_PROC_NULL) {
                unsigned i = chunck[COORDINATE::X] - 1;
                unsigned j = chunck[COORDINATE::Y] - 1;
                for (unsigned k = 1; k < chunck[COORDINATE::Z] - 1; ++k)
                    T[i][j][k] = 2.0 * T[i - 1][j][k] - T[i - 2][j][k];
            }
            if (neighbors[DIRECTION::BACK] != MPI_PROC_NULL) {
                unsigned i = chunck[COORDINATE::X] - 1;
                unsigned k = 0;
                for (unsigned j = 1; j < chunck[COORDINATE::Y] - 1; ++j)
                    T[i][j][k] = 2.0 * T[i - 1][j][k] - T[i - 2][j][k];
            }
            if (neighbors[DIRECTION::FRONT] != MPI_PROC_NULL) {
                unsigned i = chunck[COORDINATE::X] - 1;
                unsigned k = chunck[COORDINATE::Z] - 1;
                for (unsigned j = 1; j < chunck[COORDINATE::Y] - 1; ++j)
                    T[i][j][k] = 2.0 * T[i - 1][j][k] - T[i - 2][j][k];
            }
        }

        if (neighbors[DIRECTION::BACK] != MPI_PROC_NULL) {
            if (neighbors[DIRECTION::BOTTOM] != MPI_PROC_NULL) {
                unsigned j = 0;
                unsigned k = 0;
                for (unsigned i = 1; i < chunck[COORDINATE::X] - 1; ++i)
                    T[i][j][k] = 2.0 * T[i][j][k + 1] - T[i][j][k + 2];
            }
            if (neighbors[DIRECTION::TOP] != MPI_PROC_NULL) {
                unsigned j = chunck[COORDINATE::Y] - 1;
                unsigned k = 0;
                for (unsigned i = 1; i < chunck[COORDINATE::X] - 1; ++i)
                    T[i][j][k] = 2.0 * T[i][j][k + 1] - T[i][j][k + 2];
            }
        }

        if (neighbors[DIRECTION::FRONT] != MPI_PROC_NULL) {
            if (neighbors[DIRECTION::BOTTOM] != MPI_PROC_NULL) {
                unsigned j = 0;
                unsigned k = chunck[COORDINATE::Z] - 1;
                for (unsigned i = 1; i < chunck[COORDINATE::X] - 1; ++i)
                    T[i][j][k] = 2.0 * T[i][j][k - 1] - T[i][j][k - 2];
            }
            if (neighbors[DIRECTION::TOP] != MPI_PROC_NULL) {
                unsigned j = chunck[COORDINATE::Y] - 1;
                unsigned k = chunck[COORDINATE::Z] - 1;
                for (unsigned i = 1; i < chunck[COORDINATE::X] - 1; ++i)
                    T[i][j][k] = 2.0 * T[i][j][k - 1] - T[i][j][k - 2];
            }
        }
        /// finished with halo edges extrapolation
        /// 完成光晕边缘外推

        /// at last, we update the boundary points through weighted averages
        /// 最后，我们通过加权平均值更新边界点
        if ((neighbors[DIRECTION::LEFT] != MPI_PROC_NULL) && (neighbors[DIRECTION::BOTTOM] != MPI_PROC_NULL) &&
            (neighbors[DIRECTION::BACK] != MPI_PROC_NULL)) {
            unsigned i = 0;
            unsigned j = 0;
            unsigned k = 0;
            T[i][j][k] = 1.0 / 3.0 * (T[i + 1][j][k] + T[i][j + 1][k] + T[i][j][k + 1]);
        }

        if ((neighbors[DIRECTION::LEFT] != MPI_PROC_NULL) && (neighbors[DIRECTION::BOTTOM] != MPI_PROC_NULL) &&
            (neighbors[DIRECTION::FRONT] != MPI_PROC_NULL)) {
            unsigned i = 0;
            unsigned j = 0;
            unsigned k = chunck[COORDINATE::Z] - 1;
            T[i][j][k] = 1.0 / 3.0 * (T[i + 1][j][k] + T[i][j + 1][k] + T[i][j][k - 1]);
        }

        if ((neighbors[DIRECTION::LEFT] != MPI_PROC_NULL) && (neighbors[DIRECTION::TOP] != MPI_PROC_NULL) &&
            (neighbors[DIRECTION::BACK] != MPI_PROC_NULL)) {
            unsigned i = 0;
            unsigned j = chunck[COORDINATE::Y] - 1;
            unsigned k = 0;
            T[i][j][k] = 1.0 / 3.0 * (T[i + 1][j][k] + T[i][j - 1][k] + T[i][j][k + 1]);
        }

        if ((neighbors[DIRECTION::LEFT] != MPI_PROC_NULL) && (neighbors[DIRECTION::TOP] != MPI_PROC_NULL) &&
            (neighbors[DIRECTION::FRONT] != MPI_PROC_NULL)) {
            unsigned i = 0;
            unsigned j = chunck[COORDINATE::Y] - 1;
            unsigned k = chunck[COORDINATE::Z] - 1;
            T[i][j][k] = 1.0 / 3.0 * (T[i + 1][j][k] + T[i][j - 1][k] + T[i][j][k - 1]);
        }

        if ((neighbors[DIRECTION::RIGHT] != MPI_PROC_NULL) && (neighbors[DIRECTION::BOTTOM] != MPI_PROC_NULL) &&
            (neighbors[DIRECTION::BACK] != MPI_PROC_NULL)) {
            unsigned i = chunck[COORDINATE::X] - 1;
            unsigned j = 0;
            unsigned k = 0;
            T[i][j][k] = 1.0 / 3.0 * (T[i - 1][j][k] + T[i][j + 1][k] + T[i][j][k + 1]);
        }

        if ((neighbors[DIRECTION::RIGHT] != MPI_PROC_NULL) && (neighbors[DIRECTION::BOTTOM] != MPI_PROC_NULL) &&
            (neighbors[DIRECTION::FRONT] != MPI_PROC_NULL)) {
            unsigned i = chunck[COORDINATE::X] - 1;
            unsigned j = 0;
            unsigned k = chunck[COORDINATE::Z] - 1;
            T[i][j][k] = 1.0 / 3.0 * (T[i - 1][j][k] + T[i][j + 1][k] + T[i][j][k - 1]);
        }

        if ((neighbors[DIRECTION::RIGHT] != MPI_PROC_NULL) && (neighbors[DIRECTION::TOP] != MPI_PROC_NULL) &&
            (neighbors[DIRECTION::BACK] != MPI_PROC_NULL)) {
            unsigned i = chunck[COORDINATE::X] - 1;
            unsigned j = chunck[COORDINATE::Y] - 1;
            unsigned k = 0;
            T[i][j][k] = 1.0 / 3.0 * (T[i - 1][j][k] + T[i][j - 1][k] + T[i][j][k + 1]);
        }

        if ((neighbors[DIRECTION::RIGHT] != MPI_PROC_NULL) && (neighbors[DIRECTION::TOP] != MPI_PROC_NULL) &&
            (neighbors[DIRECTION::FRONT] != MPI_PROC_NULL)) {
            unsigned i = chunck[COORDINATE::X] - 1;
            unsigned j = chunck[COORDINATE::Y] - 1;
            unsigned k = chunck[COORDINATE::Z] - 1;
            T[i][j][k] = 1.0 / 3.0 * (T[i - 1][j][k] + T[i][j - 1][k] + T[i][j][k - 1]);
        }
        /// finished with halo corner points
        /// 带有光晕角点


/// calculate the difference between the current and previous (last time step) solution.
  /// 计算当前解决方案与上一个（最后一步）解决方案之间的差异。

        floatT res = std::numeric_limits<floatT>::min();
        for (unsigned i = 1; i < chunck[COORDINATE::X] - 1; ++i)
            for (unsigned j = 1; j < chunck[COORDINATE::Y] - 1; ++j)
                for (unsigned k = 1; k < chunck[COORDINATE::Z] - 1; ++k)
                    if (std::fabs(T[i][j][k] - T0[i][j][k]) > res)
                        res = std::fabs(T[i][j][k] - T0[i][j][k]);

        /// if it is the first time step, store the residual as the normalisation factor
        /// 如果是第一步，则将残差存储为归一化因子


        if (time == 0)
            if (res != 0.0)
                norm = res;

        /// For MPI, we have to communicate the norm by selecting the lowest among all processors
        /// 对于MPI，我们必须通过选择所有处理器中的最低处理器来传达规范

        if (time == 0) {
            MPI_Iallreduce(&norm, &globalNorm, 1, MPI_FLOAT_T, MPI_MIN, MPI_COMM_CART, &reduceRequest);
            MPI_Wait(&reduceRequest, MPI_STATUS_IGNORE);
        }


        /// if we want to debug, it may be useful to see the residuals. Turned of for release builds for performance.
          /// 如果我们要调试，查看残差可能会很有用。 为性能而发布版本。
//#if defined(USE_DEBUG)
//        if (rank == 0) {
//            std::cout << "time: " << std::setw(10) << time;
//            std::cout << std::scientific << std::setw(15) << std::setprecision(5) << ", residual: ";
//            std::cout << res / norm << std::endl;
//        }
//#endif

        /// check if the current residual has dropped below our defined convergence threshold "eps"
          /// 检查当前残差是否已降至我们定义的收敛阈值“ eps”以下
        if (res / norm < eps)
            breakCondition = true;

        /// Again, for MPI we need to among all processors if we can break from the loop
        /// 同样，对于MPI，如果我们可以中断循环，则需要进入所有处理器


        MPI_Iallreduce(&breakCondition, &globalBreakCondition, 1, MPI_INT, MPI_MAX, MPI_COMM_CART, &reduceRequest);
        MPI_Wait(&reduceRequest, MPI_STATUS_IGNORE);


        /// final check if we can break, the above was just preparation for this check.
          /// 最后检查我们是否可以休息，以上只是准备检查的方法


        if (globalBreakCondition) {
            finalNumIterations = time;
            break;
        }
    }
    /// done with the time loop
    /// 完成时间循环

    /// output the timing information to screen.
    /// 将定时信息输出到屏幕。

    auto end = MPI_Wtime();
    if (rank == 0) {
        std::cout << "Computational time (parallel): " << std::fixed << (end - start) << "\n" << std::endl;
        if (globalBreakCondition) {
            std::cout << "Simulation has converged in " << finalNumIterations << " iterations";
            std::cout << " with a convergence threshold of " << std::scientific << eps << std::endl;
        }
        else
            std::cout << "Simulation did not converge within " << iterMax << " iterations." << std::endl;
    }


    /// calculate the error we have made against the analytic solution
      /// 计算我们针对解析解所做的误差

    double globalError = 0.0;
    double error = 0.0;
    for (unsigned k = 1; k < chunck[COORDINATE::Z] - 1; ++k)
        for (unsigned j = 1; j < chunck[COORDINATE::Y] - 1; ++j)
            for (unsigned i = 1; i < chunck[COORDINATE::X] - 1; ++i)
                error += std::sqrt(std::pow(T[i][j][k] - (coordinates3D[COORDINATE::Y] * (chunck[COORDINATE::Y] - 1) + j) * spacing[COORDINATE::Y], 2.0));
    error /= ((chunck[COORDINATE::X] - 2) * (chunck[COORDINATE::Y] - 2) * (chunck[COORDINATE::Z] - 2));
    MPI_Iallreduce(&error, &globalError, 1, MPI_FLOAT_T, MPI_SUM, MPI_COMM_CART, &reduceRequest);
    MPI_Wait(&reduceRequest, MPI_STATUS_IGNORE);
    if (rank == 0)
        std::cout << "L2-norm error: " << std::fixed << std::setprecision(4) << 100 * error << " %" << std::endl;


    /// output the solution in a format readable by a post processor, such as paraview.
      /// 以后处理器可读的格式（例如paraview）输出解决方案。

    std::vector<floatT> receiveBufferPostProcess;
    receiveBufferPostProcess.resize(chunck[COORDINATE::X] * chunck[COORDINATE::Y] * chunck[COORDINATE::Z]);
    if (rank > 0 && size != 1)
    {
        int counter = 0;
        for (unsigned k = 0; k < chunck[COORDINATE::Z]; ++k)
            for (unsigned j = 0; j < chunck[COORDINATE::Y]; ++j)
                for (unsigned i = 0; i < chunck[COORDINATE::X]; ++i)
                    receiveBufferPostProcess[counter++] = T[i][j][k];

        MPI_Send(&receiveBufferPostProcess[0], chunck[COORDINATE::X] * chunck[COORDINATE::Y] * chunck[COORDINATE::Z], MPI_FLOAT_T, 0, 200 + rank, MPI_COMM_CART);
        MPI_Send(&coordinates3D[0], NUMBER_OF_DIMENSIONS, MPI_INT, 0, 300 + rank, MPI_COMM_CART);
    }
    if (rank == 0 && size != 1)
    {
        std::ofstream out("output/out.dat");
        out << "TITLE=\"out\"" << std::endl;
        out << "VARIABLES = \"X\", \"Y\", \"Z\", \"T\", \"rank\"" << std::endl;
        out << "ZONE T = \"" << rank << "\", I=" << chunck[COORDINATE::X] << ", J=" << chunck[COORDINATE::Y] << ", K=" << chunck[COORDINATE::Z] << ", F=POINT" << std::endl;
        for (unsigned k = 0; k < chunck[COORDINATE::Z]; ++k)
            for (unsigned j = 0; j < chunck[COORDINATE::Y]; ++j)
                for (unsigned i = 0; i < chunck[COORDINATE::X]; ++i)
                {
                    out << std::scientific << std::setprecision(5) << std::setw(15) << (coordinates3D[COORDINATE::X] * (chunck[COORDINATE::X] - 1) + i) * spacing[COORDINATE::X];
                    out << std::scientific << std::setprecision(5) << std::setw(15) << (coordinates3D[COORDINATE::Y] * (chunck[COORDINATE::Y] - 1) + j) * spacing[COORDINATE::Y];
                    out << std::scientific << std::setprecision(5) << std::setw(15) << (coordinates3D[COORDINATE::Z] * (chunck[COORDINATE::Z] - 1) + k) * spacing[COORDINATE::Z];
                    out << std::scientific << std::setprecision(5) << std::setw(15) << T[i][j][k];
                    out << std::fixed << std::setw(5) << rank << std::endl;
                }

        for (int recvRank = 1; recvRank < size; ++recvRank)
        {
            int coordinates3DFromReceivedRank[NUMBER_OF_DIMENSIONS];
            MPI_Recv(&receiveBufferPostProcess[0], chunck[COORDINATE::X] * chunck[COORDINATE::Y] * chunck[COORDINATE::Z], MPI_FLOAT_T, recvRank, 200 + recvRank, MPI_COMM_CART, &postStatus[0]);
            MPI_Recv(&coordinates3DFromReceivedRank[0], NUMBER_OF_DIMENSIONS, MPI_INT, recvRank, 300 + recvRank, MPI_COMM_CART, &postStatus[1]);

            out << "ZONE T = \"" << rank << "\", I=" << chunck[COORDINATE::X] << ", J=" << chunck[COORDINATE::Y] << ", K=" << chunck[COORDINATE::Z] << ", F=POINT" << std::endl;
            int counter = 0;
            for (unsigned k = 0; k < chunck[COORDINATE::Z]; ++k)
                for (unsigned j = 0; j < chunck[COORDINATE::Y]; ++j)
                    for (unsigned i = 0; i < chunck[COORDINATE::X]; ++i)
                    {
                        out << std::scientific << std::setprecision(5) << std::setw(15) << (coordinates3DFromReceivedRank[COORDINATE::X] * (chunck[COORDINATE::X] - 1) + i) * spacing[COORDINATE::X];
                        out << std::scientific << std::setprecision(5) << std::setw(15) << (coordinates3DFromReceivedRank[COORDINATE::Y] * (chunck[COORDINATE::Y] - 1) + j) * spacing[COORDINATE::Y];
                        out << std::scientific << std::setprecision(5) << std::setw(15) << (coordinates3DFromReceivedRank[COORDINATE::Z] * (chunck[COORDINATE::Z] - 1) + k) * spacing[COORDINATE::Z];
                        out << std::scientific << std::setprecision(5) << std::setw(15) << receiveBufferPostProcess[counter++];
                        out << std::fixed << std::setw(5) << recvRank << std::endl;
                    }
        }
        out.close();
    }
    if (size == 1)
    {
        std::ofstream out("output/out.dat");
        out << "TITLE=\"out\"" << std::endl;
        out << "VARIABLES = \"X\", \"Y\", \"Z\", \"T\"" << std::endl;
        out << "ZONE T = \"" << rank << "\", I=" << chunck[COORDINATE::X] << ", J=" << chunck[COORDINATE::Y] << ", K=" << chunck[COORDINATE::Z] << ", F=POINT" << std::endl;
        for (unsigned k = 0; k < chunck[COORDINATE::Z]; ++k)
            for (unsigned j = 0; j < chunck[COORDINATE::Y]; ++j)
                for (unsigned i = 0; i < chunck[COORDINATE::X]; ++i)
                {
                    out << std::scientific << std::setprecision(5) << std::setw(15) << (coordinates3D[COORDINATE::X] * (chunck[COORDINATE::X] - 1) + i) * spacing[COORDINATE::X];
                    out << std::scientific << std::setprecision(5) << std::setw(15) << (coordinates3D[COORDINATE::Y] * (chunck[COORDINATE::Y] - 1) + j) * spacing[COORDINATE::Y];
                    out << std::scientific << std::setprecision(5) << std::setw(15) << (coordinates3D[COORDINATE::Z] * (chunck[COORDINATE::Z] - 1) + k) * spacing[COORDINATE::Z];
                    out << std::scientific << std::setprecision(5) << std::setw(15) << T[i][j][k] << std::endl;
                }
        out.close();
    }



    MPI_Finalize();

    return 0;
}