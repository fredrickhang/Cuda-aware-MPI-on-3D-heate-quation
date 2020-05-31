



__global__  void computeT(double*** TBegin, double*** TEnd, int numX, int numY, int numZ, double Dx, double Dy, double Dz) {

    for (unsigned i = 1; i < numX - 1; ++i)
        for (unsigned j = 1; j < numY - 1; ++j)
            for (unsigned k = 1; k < numZ - 1; ++k) {
                TEnd[i][j][k] = TBegin[i][j][k] +
                    Dx * (TBegin[i + 1][j][k] - 2.0 * TBegin[i][j][k] + TBegin[i - 1][j][k]) +
                    Dy * (TBegin[i][j + 1][k] - 2.0 * TBegin[i][j][k] + TBegin[i][j - 1][k]) +
                    Dz * (TBegin[i][j][k + 1] - 2.0 * TBegin[i][j][k] + TBegin[i][j][k - 1]);
            }


}

