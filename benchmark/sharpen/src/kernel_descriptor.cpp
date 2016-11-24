#include "kernel_descriptor.hpp"

#define PRINT_ERRORS            1

extern unsigned int *code; // binary storde in code.c as an array

template<typename T>
kernel<T>::kernel(unsigned maxDim)
{
  param1 = new T[maxDim*maxDim];
  target_fgpu = new T[maxDim*maxDim];
  target_arm = new T[maxDim*maxDim];
}
template<typename T>
kernel<T>::~kernel() 
{
  delete[] param1;
  delete[] target_fgpu;
  delete[] target_arm;
}
template<typename T>
void kernel<T>::download_code()
{
  volatile unsigned *cram_ptr = (unsigned *)(FGPU_BASEADDR+ 0x4000);
  unsigned int size = SHARPEN_LEN;
  if (typeid(T) == typeid(unsigned))
    start_addr = SHARPEN5X5_POS;
  else
    assert(0 && "unsupported type");
  unsigned i = 0;
  for(; i < size; i++){
    cram_ptr[i] = code[i];
  }
}
template<typename T>
void kernel<T>::download_descriptor()
{
  int i;
  volatile unsigned* lram_ptr = (unsigned*)FGPU_BASEADDR;
  for(i = 0; i < 32; i++)
    lram_ptr[i] = 0;
  lram_ptr[0] = ((nWF_WG-1) << 28) | (0 << 14) | (start_addr);
  lram_ptr[1] = size0;
  lram_ptr[2] = size1;
  lram_ptr[3] = size2;
  lram_ptr[4] = offset0;
  lram_ptr[5] = offset1;
  lram_ptr[6] = offset2;
  lram_ptr[7] = ((nDim-1) << 30) | (wg_size2 << 20) | (wg_size1 << 10) | (wg_size0);
  lram_ptr[8] = n_wg0-1;
  lram_ptr[9] = n_wg1-1;
  lram_ptr[10] = n_wg2-1;
  lram_ptr[11] = (nParams << 28) | wg_size;
  lram_ptr[16] = (unsigned) param1;
  lram_ptr[17] = (unsigned) target_fgpu;
}
template<typename T>
void kernel<T>::prepare_descriptor(unsigned int Size)
{
  wg_size0 = 8;
  wg_size1 = 8;
  rowLen = Size;
  problemSize = Size*Size;
  offset0 = 0;
  offset1 = 0;
  nDim = 2;
  size0 = Size;
  size1 = Size;
  dataSize = 4 * problemSize; // 4 bytes per word

  if(size0 < 8)
    wg_size0 = size0;
  if(size1 < 8)
    wg_size1 = size1;
  if(Size>=64) {
    wg_size0 = 64;
    wg_size1 = 1;
  } else if(Size >=32) {
    wg_size0 = 32;
    wg_size1 = 2;
  }
  compute_descriptor();

  offset0 = offset1 = offset2 = 0;
  nParams = 2; // number of parameters
}
template<typename T>
void kernel<T>::initialize_memory()
{
  unsigned i;
  T *param1_ptr = (T*) param1;
  T *target_ptr = (T*) target_fgpu;
  srand(1);
  for(i = 0; i < problemSize; i++) 
  {
    param1_ptr[i] = 5;
    // param1_ptr[i] = rand()&0xFF;
    target_ptr[i] = 0;
  }
  Xil_DCacheFlush(); // flush data to global memory
}
template<typename T>
unsigned kernel<T>::compute_on_ARM(unsigned int n_runs)
{
  unsigned i, j;
  unsigned exec_time = 0;
  unsigned runs = 0;

  XTime tStart, tEnd;
  while(runs < n_runs)
  {
    initialize_memory();
    Xil_DCacheFlush();
    Xil_DCacheInvalidate();

    // parametrs accessed during computations should be cashed
    T *target_ptr = target_arm;
    T *param1_ptr = param1;
    unsigned Size = size0;
    // printf("size0 = %d\n", Size);

    XTime_GetTime(&tStart);

    for(i = 2; i < Size-2; i++)
    {
      for(j = 2;j < Size-2; j++)
      {
        unsigned row, col, res;
        unsigned p[5][5];
        for(row = 0; row < 5; row++) {
          for(col = 0; col < 5; col++) {
            p[row][col] = param1_ptr[(i+row-2)*Size+j+col-2];
          }
        }


        res = -1*p[0][0] -1*p[0][1] -1*p[0][2] -1*p[0][3] -1*p[0][4] +
              -1*p[1][0] +2*p[1][1] +2*p[1][2] +2*p[1][3] -1*p[1][4] +
              -1*p[2][0] +2*p[2][1] +8*p[2][2] +2*p[2][3] -1*p[2][4] +
              -1*p[3][0] +2*p[3][1] +2*p[3][2] +2*p[3][3] -1*p[3][4] +
              -1*p[4][0] -1*p[4][1] -1*p[4][2] -1*p[4][3] -1*p[4][4];

        target_ptr[i*Size+j] = res/8;
      }
    }
      
    // flush the results to the global memory 
    // If the size of the data to be flushed exceeds half of the cache size, flush the whole cache. It is faster!
    if (dataSize > 16*1024)
      Xil_DCacheFlush();
    else
      Xil_DCacheFlushRange((unsigned)target_arm, dataSize);
    
    XTime_GetTime(&tEnd);
    exec_time += elapsed_time_us(tStart, tEnd);
    xil_printf(ANSI_COLOR_RED "." ANSI_COLOR_RESET);
    fflush(stdout);
    runs++;
    if(exec_time > 1000000*MAX_MES_TIME_S)
      break;
  }
  return exec_time/runs;
}
template<typename T>
void kernel<T>::check_FGPU_results()
{
  unsigned i, j, nErrors = 0;
  // xil_printf("original matrix:\n\r");
  // for(i = 0; i < rowLen; i++) {
  //   for(j = 0; j < rowLen; j++) {
  //     xil_printf("%12d", param1[i*rowLen+j]);
  //   }
  //   xil_printf("\n\r");
  // }
  // xil_printf("fgpu matrix:\n\r");
  // for(i = 0; i < rowLen; i++) {
  //   for(j = 0; j < rowLen; j++) {
  //     xil_printf("%12d", target_fgpu[i*rowLen+j]);
  //   }
  //   xil_printf("\n\r");
  // }
  // xil_printf("arm matrix:\n\r");
  // for(i = 0; i < rowLen; i++) {
  //   for(j = 0; j < rowLen; j++) {
  //     xil_printf("%12d", target_arm[i*rowLen+j]);
  //   }
  //   xil_printf("\n\r");
  // }

  for (i = 2; i < rowLen-2; i++) 
  {
    for (j = 2; j < rowLen-2; j++)
    {
      if((target_arm[i*rowLen+j]&0x00FFFFFF) != (target_fgpu[i*rowLen+j]&0x00FFFFFF))
      {
        #if PRINT_ERRORS
        if(nErrors < 50)
          xil_printf("res[0x%x]=0x%x (must be 0x%x)\n\r", i*rowLen+j, (unsigned)target_fgpu[i*rowLen + j], (unsigned) target_arm[i*rowLen + j]);
        #endif
        nErrors++;
      }
    }
  }
  if(nErrors != 0) {
    xil_printf("Memory check failed (nErrors = %d)!\n\r", nErrors);
  }
}
template<typename T>
unsigned kernel<T>::compute_on_FGPU(unsigned n_runs, bool check_results)
{
  unsigned runs = 0;
  XTime tStart, tEnd;
  unsigned exec_time = 0;

  while(runs < n_runs)
  {
    initialize_memory();
    download_descriptor();
    REG_WRITE(INITIATE_REG_ADDR, 0xFFFF); // initiate FGPU when execution starts
    REG_WRITE(CLEAN_CACHE_REG_ADDR, 0xFFFF); // clean FGPU cache at end of execution
    
    XTime_GetTime(&tStart);
    REG_WRITE(START_REG_ADDR, 1);
    while(REG_READ(STATUS_REG_ADDR)==0);
    XTime_GetTime(&tEnd);
    exec_time += elapsed_time_us(tStart, tEnd);
    
    if(check_results)
      check_FGPU_results();

    xil_printf(ANSI_COLOR_GREEN "." ANSI_COLOR_RESET);
    fflush(stdout);
    runs++;

    if(exec_time > 1000000*MAX_MES_TIME_S)// do not execute all required runs if it took too long
      break;
  }
  return exec_time/runs;
}
template<typename T>
void kernel<T>::print_name()
{
  xil_printf("\n\r" ANSI_COLOR_YELLOW "Kernel is sharpen word\n\r" ANSI_COLOR_RESET);
}
template<typename T>
unsigned kernel<T>::get_problemSize() 
{
  return problemSize;
}
template<typename T>
void kernel<T>::compute_descriptor()
{
  assert(wg_size0 > 0 && wg_size0 <= 512);
  assert(size0 % wg_size0 == 0);
  size = size0;
  wg_size = wg_size0;
  n_wg0 = size0 / wg_size0;
  if(nDim > 1)
  {
    assert(wg_size1 > 0 && wg_size1 <= 512);
    assert(size1 % wg_size1 == 0);
    size = size0 * size1;
    wg_size = wg_size0 * wg_size1;
    n_wg1 = size1 / wg_size1;
  }
  else
  {
    wg_size1 = n_wg1 = size1 = 0;
  }
  if(nDim > 2)
  {
    assert(wg_size2 > 0 && wg_size2 <= 512);
    assert(size2 % wg_size2 == 0);
    size = size0 * size1 * size2;
    wg_size = wg_size0 * wg_size1 * wg_size2;
    n_wg2 = size2 / wg_size2;
  }
  else
  {
    wg_size2 = n_wg2 = size2 = 0;
  }
  assert(wg_size <= 512);
  nWF_WG = wg_size / 64;
  if(wg_size % 64 != 0)
    nWF_WG++;
}

template class kernel<unsigned>;