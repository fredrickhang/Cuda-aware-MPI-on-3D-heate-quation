# Compilers
MPICC=$(PREP) mpicxx
MPILD=$(PREP) mpic++
NVCC=$(PREP) nvcc

# Flags
CFLAGS=-std=c99 -O3 -march=native -Wall
MPICFLAGS=-I${MPI_HOME}/include
CUDACFLAGS=-I${CUDA_INSTALL_PATH}/include

GENCODE_SM30    := -gencode arch=compute_30,code=sm_30
GENCODE_SM35    := -gencode arch=compute_35,code=sm_35
GENCODE_SM35    := -gencode arch=compute_37,code=sm_37
GENCODE_SM50    := -gencode arch=compute_50,code=sm_50
GENCODE_SM52    := -gencode arch=compute_52,code=sm_52
GENCODE_SM60    := -gencode arch=compute_60,code=sm_60
GENCODE_SM70    := -gencode arch=compute_70,code=\"sm_70,compute_70\"
GENCODE_FLAGS   := $(GENCODE_SM30) $(GENCODE_SM35) $(GENCODE_SM37)             \
$(GENCODE_SM50) $(GENCODE_SM52) $(GENCODE_SM60) $(GENCODE_SM70)

NVCCFLAGS=-O3 $(GENCODE_FLAGS) -Xcompiler -march=native

CUDALDFLAGS=-L${CUDA_INSTALL_PATH}/lib64 -lcudart

# Description of binaries
BINDIR=../bin
JACOBI_CUDA_NORMAL_MPI=$(BINDIR)
BINARIES=$(JACOBI_CUDA_NORMAL_MPI) 

# Commands
all: $(BINARIES)

device.o: cudaCode.cu Makefile
	$(NVCC) $(MPICFLAGS) $(NVCCFLAGS) -c cudaCode.cu -o cudaCode.o

input.o: heat3D.cpp Makefile
	$(MPICC) $(MPICFLAGS) $(CUDACFLAGS) $(CFLAGS) -c heat3D.cpp -o heat3D.o


$(JACOBI_CUDA_NORMAL_MPI): cudaCode.o heat3D.o Makefile
	mkdir -p $(BINDIR)
	$(MPILD) -o $(JACOBI_CUDA_NORMAL_MPI) cudaCode.o heat3D.o $(CUDALDFLAGS)
	
doc: cudaCode.cu heat3D.cpp 	
clean:
	rm -rf *.o *~ $(BINARIES)