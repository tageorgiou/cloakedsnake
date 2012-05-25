/***
* Compile with :
*   gcc -O3 -funroll-loops -o hashfunctionsreal hashfunctionsreal.c
*   for some compilers (e.g., GCC 4.4) on x86-64 we need -mno-sse2 to avoid 
*    poor performance due to bad sse instructions.
*
* New: For Galois Field and carryless multiplications, try this command line:
* 
* gcc -O3 -funroll-loops -Wall -maes -msse4 -mpclmul  -o hashfunctionsreal hashfunctionsreal.c
*
*
*/


#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>


/* code these things in plain C to avoid surprises... */

typedef unsigned int uint32;
typedef unsigned long long uint64;
typedef unsigned long long ticks;



#if defined(LITTLEMEMORY) || defined(IPAD)
#define N 1024* 32 // should be divisible by two!
#else
#define N 1024* 64 // should be divisible by two!
#endif

#ifdef IPAD
#define TRIALS 20000
#else
#define TRIALS 20000
#endif

#define SHORTN  1024 // should fit in L1 cache
#ifdef IPAD
#define SHORTTRIALS 100000
#else
#define SHORTTRIALS 1000000
#endif


#if defined( __corei7__  )  // __amd64__ is untested

// start and stop are as recommended by 
// Gabriele Paoloni, How to Benchmark Code Execution Times on Intel® IA-32 and IA-64 Instruction Set Architectures
// September 2010
// http://edc.intel.com/Link.aspx?id=3954

static __inline__ ticks startRDTSC (void) {
  unsigned cycles_low, cycles_high;
  asm volatile ("CPUID\n\t"
		"RDTSC\n\t"
		"mov %%edx, %0\n\t"
		"mov %%eax, %1\n\t": "=r" (cycles_high), "=r" (cycles_low)::
		"%rax", "%rbx", "%rcx", "%rdx");
  return ((ticks)cycles_high << 32) | cycles_low;
}

static __inline__ ticks stopRDTSCP (void) {
  unsigned cycles_low, cycles_high;
/// This should work fine on most machines, if the RDTSCP thing
/// fails for you, use the  rdtsc() call instead.
  asm volatile("RDTSCP\n\t"
	       "mov %%edx, %0\n\t"
	       "mov %%eax, %1\n\t"
	       "CPUID\n\t": "=r" (cycles_high), "=r" (cycles_low):: "%rax",
	       "%rbx", "%rcx", "%rdx");
  return ((ticks)cycles_high << 32) | cycles_low;
}

#elif defined (__i386__) || defined( __x86_64__ ) 


// Taken from stackoverflow (see http://stackoverflow.com/questions/3830883/cpu-cycle-count-based-profiling-in-c-c-linux-x86-64)
// Can give nonsensical results on multi-core AMD processors.
inline uint64 rdtsc() {
    unsigned int lo, hi;
    asm volatile (
        "cpuid \n" /* serializing */
        "rdtsc"
        : "=a"(lo), "=d"(hi) /* outputs */
        : "a"(0)           /* inputs */
        : "%ebx", "%ecx");     /* clobbers*/
    return ((unsigned long long)lo) | (((unsigned long long)hi) << 32);
}
static __inline__ ticks startRDTSC (void) {
	return rdtsc();
}

static __inline__ ticks stopRDTSCP (void) {
	return rdtsc();
}

#elif ( defined(__arm__) || defined(__ppc__) || defined(__ppc64__) )
 
// for PPC we should be able to use tbl, but I could not find
// an equivalent to rdtsc for ARM.

inline uint64 rdtsc() { return 0; }
static __inline__ ticks startRDTSC (void) {return 0;}
static __inline__ ticks stopRDTSCP (void) {return 0;}
#else
#error Unknown architecture
#endif


// multilinear, 2-by-2
uint64 hashSM2b2(const uint64 *  randomsource, const uint32 *  string, const uint32 * const endstring) {
    uint64 sum = *(randomsource++);
    for(; string!= endstring; randomsource+=2,string+=2 ) {
        sum+= (*randomsource *  (uint64)(*string)) + (*(randomsource + 1) *  (uint64)(*(string+1)));
    }
    return (int) (sum>>32);
}

// proposed by Patrascu and Thorup (STOC 2010)
// warning: may use more memory than the other guys and correspondingly,
// we may have a buffer overrun if we are not careful.
uint64 hashXAMA(const uint64 *  randomsource, const uint32 *  string, const uint32 * const endstring) {
    uint64 sum = 0;
    for(; string!= endstring; randomsource+=3,string+=2 ) {
        sum^= (*randomsource +  (uint64)(*string)) * (*(randomsource + 1) +  (uint64)(*(string+1))) + *(randomsource + 2);
    }
    return (int) (sum>>32);
}

// 
uint64 hashMultilinearHalfMultiplications(const uint64 *  randomsource, const uint32 *  string, const uint32 * const endstring) {
    uint64 sum = *(randomsource++);
    for(; string!= endstring; randomsource+=2,string+=2 ) {
        sum+= (*randomsource +  (uint64)(*string)) * (*(randomsource + 1) +  (uint64)(*(string+1)));
    }
    return (int) (sum>>32);
}

//Black, J.; Halevi, S.; Krawczyk, H.; Krovetz, T. (1999). "UMAC: Fast and Secure Message Authentication". Advances in Cryptology (CRYPTO '99)., Equation 1
uint64 hashNH(const uint64 *  randomsource, const uint32 *  string, const uint32 * const endstring) {
    uint64 sum = 0;
    const uint32 *  randomsource32 = ( const uint32 * )randomsource;
    for(; string!= endstring; randomsource32+=2,string+=2 ) {
        sum+= ((uint64)(*randomsource32+ (*string))) * ((uint64)(*(randomsource32 + 1) + (*(string+1))));
    }
    return sum;
}



// multilinear
uint64 hashMultilinear(const uint64 *  randomsource, const uint32 *  string, const uint32 * const endstring) {
    uint64 sum = *(randomsource++);
    for(; string!= endstring; ++randomsource,++string ) {
        sum+= (*randomsource *  (uint64)(*string)) ;
    }
    return (int) (sum>>32);
}


// multilinear based on squaring
uint64 hashMultilinearBasedOnSquares(const uint64 *  randomsource, const uint32 *  string, const uint32 * const endstring) {
    uint64 sum = *(randomsource++);
    for(; string!= endstring; ++randomsource,++string ) {
    	const uint64 tmp = *randomsource +  (uint64)(*string);
        sum+= tmp * tmp;
    }
    return (int) (sum>>32);
}


// linear
uint64 hashLinear(const uint64 *  randomsource, const uint32 *  string, const uint32 * const endstring) {
    uint64 sum = 0;
    for(; string!= endstring; randomsource+=2,++string ) {
        sum^= (*randomsource *  (uint64)(*string) + *(randomsource+1) ) ;
    }
    return (int) (sum>>32);
}

// 
uint64 hashMultilinearWithFour(const uint64 *  randomsource, const uint32 *  string, const uint32 * const endstring) {
    uint64 sum1 = *(randomsource++);
    uint64 sum2 = 0;
    uint64 sum3 = 0;
    uint64 sum4 = 0;
    for(; string!= endstring; randomsource+=4,string+=4 ) {
        sum1 += (*randomsource *  (uint64)(*string));
        sum2 += (*(randomsource+1) *  (uint64)(*(string+1)));
        sum3 += (*(randomsource+2) *  (uint64)(*(string+2)));
        sum4 += (*(randomsource+3) *  (uint64)(*(string+3)));
    }
    return (int) ((sum1+sum2+sum3+sum4)>>32);
}
// 
uint64 hashMultilinearWithTwo(const uint64 *  randomsource, const uint32 *  string, const uint32 * const endstring) {
    uint64 sum1 = *(randomsource++);
    uint64 sum2 = 0;
    for(; string!= endstring; randomsource+=2,string+=2 ) {
        sum1 += (*randomsource *  (uint64)(*string));
        sum2 += (*(randomsource+1) *  (uint64)(*(string+1)));
        
    }
    return (int) ((sum1+sum2)>>32);
}

// Rabin-Karp Hashing
uint64 hashRabinKarp(const uint64 * randomsource , const uint32 *  string, const uint32 * const endstring) {
    int sum = 0;
    const int someprime = 31;
    for(; string!= endstring; ++string ) {
        sum= someprime * sum +  *string ;
    }
    return sum;
}

// This is similar to Rabin-Karp, but we avoid the multiplication
//D. J. Bernstein, CDB–Constant Database, http://cr.yp.to/cdb.html
uint64 hashBernstein(const uint64 * randomsource,const uint32 *  string, const uint32 * const endstring) {
    int sum = 0;
    const int L = 3;
    for(; string!= endstring; ++string ) {
        sum= (( sum<< L) + sum) ^ *string ;
    }
    return sum;
}

// Fowler-Noll-Vo hashing
// L. C. Noll, Fowler/Noll/Vo Hash, http://www.isthe.com/chongo/tech/comp/fnv/
uint64 hashFNV1(const uint64 * randomsource,const uint32 *  string, const uint32 * const endstring) {
    int sum = 0;
    const int someprime = 31;
    for(; string!= endstring; ++string ) {
        sum= ( someprime * sum) ^ *string ;
    }
    return sum;
}

// Fowler-Noll-Vo hashing
// L. C. Noll, Fowler/Noll/Vo Hash, http://www.isthe.com/chongo/tech/comp/fnv/
uint64 hashFNV1a(const uint64 * randomsource ,const uint32 *  string, const uint32 * const endstring) {
    int sum = 0;
    const int someprime = 31;
    for(; string!= endstring; ++string ) {
        sum= ( (*string) ^ sum) * someprime ;
    }
    return sum;
}

// M. V. Ramakrishna, J. Zobel, Performance in practice of string hashing functions,
//in: Proc. Int. Conf. on Database Systems for Advanced Applications, 1997.
uint64 hashSAX(const uint64 * randomsource ,const uint32 *  string, const uint32 * const endstring) {
    int sum = 0;
    const int L = 3;
    const int R = 5;
    for(; string!= endstring; ++string ) {
        sum= sum ^((sum<<L)+(sum>>R)+*string) ;
    }
    return sum;
}


typedef uint64 (*pt2Function)(const uint64 *  ,const  uint32 * , const uint32 * const );



#if defined (__PCLMUL__)                                                                                                                                                      


#include "clmul.h"


#define HowManyFunctions 15


pt2Function funcArr[HowManyFunctions] = {&hashGaloisFieldMultilinearHalfMultiplications,&hashGaloisFieldMultilinear, &hashMultilinearWithFour,&hashMultilinearWithTwo,&hashSM2b2,&hashMultilinearHalfMultiplications,&hashMultilinear,&hashNH,&hashLinear,&hashRabinKarp,&hashBernstein,&hashFNV1,&hashFNV1a,&hashSAX,&hashMultilinearBasedOnSquares};

char* functionnames[HowManyFunctions] = {"Galois Field Multilinear half-multiplications  (strongly universal)","regular Galois Field Multilinear (strongly universal)","Multilinear with 4 counters (strongly universal)","Multilinear with 2 counters (strongly universal)","Multilinear 2-by-2 (strongly universal)",  "Multilinear half-multiplications  (strongly universal)","regular Multilinear (strongly universal)","HN (almost universal)","Linear (strongly universal)", "RabinKarp", "Bernstein","FNV1","FNV1a","SAX","squaring multilinear"};

#else// #if defined (__PCLMUL__)                                                                              
// "PCLMUL instructions not enabled" 


#define HowManyFunctions 13


pt2Function funcArr[HowManyFunctions] = {&hashMultilinearWithFour,&hashMultilinearWithTwo,&hashSM2b2,&hashMultilinearHalfMultiplications,&hashMultilinear,&hashNH,&hashLinear,&hashRabinKarp,&hashBernstein,&hashFNV1,&hashFNV1a,&hashSAX,&hashMultilinearBasedOnSquares};

char* functionnames[HowManyFunctions] = {"Multilinear with 4 counters (strongly universal)","Multilinear with 2 counters (strongly universal)","Multilinear 2-by-2 (strongly universal)",  "Multilinear half-multiplications  (strongly universal)","regular Multilinear (strongly universal)","HN (almost universal)","Linear (strongly universal)", "RabinKarp", "Bernstein","FNV1","FNV1a","SAX","squaring multilinear"};


#endif//#if defined (__PCLMUL__)                                                                              


  
int main(int argc, char **argv) {
    uint64 bef,aft;
    int i,j,k;
    struct timeval start, finish;
    int elapsed;
    pt2Function thisfunc;
    char * functionname;

    int sumToFoolCompiler = 0;
    const int HowManyRepeats = 3;

    uint64 randbuffer[3*N]; // could force 16-byte alignment with  __attribute__ ((aligned (16))); 
    uint32 intstring[N];// // could force 16-byte alignment with  __attribute__ ((aligned (16)));
    for (i=0; i < N; ++i) {
        randbuffer[i]=rand()| ((uint64)(rand())<<32);
        randbuffer[i+N]=rand()| ((uint64)(rand())<<32);
        randbuffer[i+2*N]=rand()| ((uint64)(rand())<<32); // up until August 5 2011, we did not initialize this memory
        intstring[i] = rand();
    }
    #if defined( __corei7__  ) 
             printf("core i7 architecture\n");
    #elif defined ( __core2__ )
             printf("core 2 architecture\n");
    #elif defined ( __corei5__ )
             printf("core i5 architecture\n");
    #elif defined ( __corei3__ )
             printf("core i3 architecture\n");
    #elif defined ( __nocona__ )
             printf("generic nocona architecture\n");
    #endif
    #if defined (_MIPS_ARCH_NOCONA)
      printf("NOCONA");
      printf("CORE2");
    #endif
    #if defined (_MIPS_ARCH)
	  printf("march=");
	  printf(_MIPS_ARCH);
	  printf("\n");    
	#endif
    #if defined (__i386__) 
      printf("i386\n");
    #elif defined( __x86_64__ )
      printf("x86_64\n");
    #elif defined(__arm__)
      printf ("ARM -- cpu clock cycles will be bogus\n");
    #elif defined(__ppc__)
      printf ("PPC -- cpu clock cycles will be bogus\n");
    #elif defined(__ppc64__)
      printf ("PPC64 -- cpu clock cycles will be bogus\n");
    #else
      #error Unknown architecture
    #endif 
    printf("GNU GCC %s \n",__VERSION__);
    #if __NO_INLINE__
      printf("__NO_INLINE__\n");
    #endif
    #if __OPTIMIZE__
      printf("__OPTIMIZE__ \n");
    #endif
    #if defined (__PCLMUL__)                                                                                                                                                      
      printf("__PCLMUL__ \n");
      printf("... initializing Galois field \n");
	  init_galois();
    #endif
	#ifdef __MMX__
	printf("__MMX__ \n");
	#endif
	#ifdef __SSE__
	printf("__SSE__ \n");
	#endif
	#ifdef __SSE2__
	printf("__SSE2__ \n");
	#endif
	#ifdef __SSE3__
	printf("__SSE3__ \n");
	#endif
	#ifdef __SSE4_2__
	printf("__SSE4_2__ \n");
	#endif
	#ifdef __SSE4_1__
	printf("__SSE4_1__ \n");
	#endif
	#ifdef __AES__
	printf("__AES__ \n");
	#endif
    printf("sizeof(uint32) = %i, sizeof(uint64)= %i \n", (int) sizeof(uint32), (int) sizeof(uint64) );
    printf("\n\n");
    if((sizeof(uint32)!=4) || (sizeof(uint64)!=8)) {
      printf("wrong number of bits");
      return -1;
    }

    printf("short strings SHORTTRIALS = %d, SHORTN = %d \n", SHORTTRIALS,SHORTN);
    for(k =0; k<HowManyRepeats; ++k) {
        printf("test #%d\n",k+1);
        for(i=0; i<HowManyFunctions; ++i) {
            thisfunc = funcArr[i];
            functionname = functionnames[i];
            gettimeofday( &start, 0);
            bef = startRDTSC();
            for(j=0; j < SHORTTRIALS; ++j)
                sumToFoolCompiler += thisfunc( &randbuffer[0],&intstring[0],&intstring[0] + SHORTN);
            aft = stopRDTSCP();
            gettimeofday( &finish, 0);  
            elapsed = ( 1000000*(finish.tv_sec - start.tv_sec) + (finish.tv_usec - start.tv_usec));
            printf("%s cycle count per element = %f  usec %d   / ignore this: %d \n",functionname,(aft-bef)*1.0/(SHORTTRIALS*SHORTN),elapsed, sumToFoolCompiler);
        }
        printf("\n");
    }

    //
    printf("longer strings TRIALS = %d, N = %d \n", TRIALS,N);
    for(k =0; k<HowManyRepeats; ++k) {
        printf("test #%d\n",k+1);
        for(i=0; i<HowManyFunctions; ++i) {
            thisfunc = funcArr[i];
            functionname = functionnames[i];
            gettimeofday( &start, 0);
            bef = startRDTSC();
            for(j=0; j < TRIALS; ++j)
                sumToFoolCompiler += thisfunc( &randbuffer[0],&intstring[0],&intstring[0] + N);
            aft = stopRDTSCP();
            gettimeofday( &finish, 0);
            elapsed = ( 1000000*(finish.tv_sec - start.tv_sec) + (finish.tv_usec - start.tv_usec));
            printf("%s cycle count per element = %f  usec %d  and sumToFoolCompiler is %d \n",functionname,(aft-bef)*1.0/(TRIALS*N),elapsed, sumToFoolCompiler);
        }
        printf("\n");
    }
    exit(0);

}
