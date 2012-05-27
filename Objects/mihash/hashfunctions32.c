
/* 16/32 bit version of hashfunctionsreal, which is 64/32 bit */

/***
* Compile with :
*   gcc -O3 -mno-mmx -DLINUX -fno-inline -o hashfunctions32 hashfunctions32.c 
*
* Run, on Linux, with taskset 1 ./hashfunctionsreal
*
*/


#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

/* code these things in plain C to avoid surprises... */

typedef unsigned short uint16;
typedef unsigned long uint32;
typedef unsigned long long uint64;

/* used on 800 MHZ Via Nehemiah */
#define CLOCKRATE 800  
/* #define N 1024*64 // should be divisible by two! */
/* #define TRIALS 20000 */
#define SHORTN  1024 // should fit in L1 cache?
#define SHORTTRIALS 1000000

int rt; // real time, microsec
int ct; // cpu time, microsec

#ifdef LINUX
struct timeval tStart;
#endif

clock_t tickStart;

void timeReset() {
#ifdef LINUX
  gettimeofday(&tStart,0);
#endif
  tickStart = clock();
}

void timeElapsed( int *real, int *cpu) {
#ifdef LINUX
  struct timeval now;
#endif
  clock_t tickNow;

#ifdef LINUX
  gettimeofday(&now,0);
#endif
  tickNow = clock();
#ifdef LINUX
  *real = 1000000*(now.tv_sec - tStart.tv_sec) + (now.tv_usec - tStart.tv_usec);
  //  printf("Times %ld %ld %ld %ld\n", now.tv_sec, tStart.tv_sec, now.tv_usec, tStart.tv_usec); 
#else
  *real = 0;
#endif
  *cpu =  1000000*((double)(tickNow - tickStart))/CLOCKS_PER_SEC;
}

// multilinear, 2-by-2
int hashSM2b2(uint32 * randomsource, uint16 * string, const uint16 * const endstring) {
  uint32 sum = *(randomsource++);
  for(; string!= endstring;randomsource+=2,string+=2 ) {
    sum+= (*randomsource *  (uint32)(*string)) + (*(randomsource + 1) *  (uint32)(*(string+1)));
  }
  return (int) (sum>>16);
}

// called hashXAMA elsewhere
int hashThorup(uint32 * randomsource, uint16 * string, const uint16 * const endstring) {
  uint32 sum = 0;
  for(; string!= endstring;randomsource+=2,string+=2 ) {
    sum^= (*randomsource +  (uint32)(*string)) * (*(randomsource + 1) +  (uint32)(*(string+1)));
  }
  return (int) (sum>>16);
}

//multilinear
int hashSM(uint32 * randomsource, uint16 * string, const uint16 * const endstring) {
  uint32 sum = *(randomsource++);
  for(; string!= endstring;++randomsource,++string ) {
    sum+= (*randomsource *  (uint32)(*string)) ;
  }
  return (int) (sum>>16);
}


int hashSilly(uint16 * string, const uint16 * const endstring) {
  int sum = 0;
  for(; string!= endstring;++string ) {
    sum= 37 * sum +  *string ;
  }
  return sum;
}

int hashSilly2by2(uint16 * string, const uint16 * const endstring) {
  int sum = 0;
  for(; string!= endstring;string+=2 ) {
    sum= 37 * ( 37 * sum +  *string ) + * (string+1);
  }
  return sum;
}

// This is similar to Silly, but we avoid the multiplication
//D. J. Bernstein, CDB–Constant Database, http://cr.yp.to/cdb.html
int hashBerstein(uint16 * string, const uint16 * const endstring) {
  int sum = 0;
  const int L = 3;
  for(; string!= endstring;++string ) {
    sum= (( sum<< L) + sum) ^ *string ; 
  }
  return sum;
}


// Rabin-Karp Hashing
uint32 hashRabinKarp(const uint16 *  string, const uint16 * const endstring) {
    int sum = 0;
    const int someprime = 31;
    for(; string!= endstring; ++string ) {
        sum= someprime * sum +  *string ;
    }
    return sum;
}


// Fowler-Noll-Vo hashing
// L. C. Noll, Fowler/Noll/Vo Hash, http://www.isthe.com/chongo/tech/comp/fnv/
int hashFNV1(uint16 * string, const uint16 * const endstring) {
  int sum = 0;
  for(; string!= endstring;++string ) {
    sum= ( 37* sum) ^ *string ; 
  }
  return sum;
}

// Fowler-Noll-Vo hashing
// L. C. Noll, Fowler/Noll/Vo Hash, http://www.isthe.com/chongo/tech/comp/fnv/
int hashFNV1a(uint16 * string, const uint16 * const endstring) {
  int sum = 0;
  for(; string!= endstring;++string ) {
    sum= ( (*string) ^ sum) * 37 ; 
  }
  return sum;
}

// M. V. Ramakrishna, J. Zobel, Performance in practice of string hashing functions,
//in: Proc. Int. Conf. on Database Systems for Advanced Applications, 1997.
int hashSAX(uint16 * string, const uint16 * const endstring) {
  int sum = 0;
  const int L = 3;
  const int R = 5;
  for(; string!= endstring;++string ) {
    sum= sum ^((sum<<L)+(sum>>R)+*string) ; 
  }
  return sum;
}

uint32 hashMultilinearHalfMultiplications(const uint32 *  randomsource, const uint16 *  string, const uint16 * const endstring) {
    uint32 sum = *(randomsource++);
    for(; string!= endstring; randomsource+=2,string+=2 ) {
        sum+= (*randomsource +  (uint32)(*string)) * (*(randomsource + 1) +  (uint32)(*(string+1)));
    }
    return (int) (sum>>16);
}



int main(int argc, char **argv) {
  //uint32 bef,aft;
  int i;//,j;
  //struct timeval start, finish;
  //int elapsed;

  int sumToFoolCompiler = 0; 
  int whichTrial;

  printf("For a clock rate of %d MHz\n",CLOCKRATE);

  for (whichTrial=0; whichTrial < 3; ++whichTrial) {
    // if she floats, she is a witch...
    printf("\n\n****************Trial %d****************\n",whichTrial);

   
  uint32 randbuffer[SHORTN+1];
  uint16 intstring[SHORTN+1];
  for (i=0; i < SHORTN+1; ++i) {randbuffer[i]=rand(); intstring[i]= (uint16) rand();}

  timeReset();
  for(i=0; i < SHORTTRIALS; ++i)
    sumToFoolCompiler += hashSM( &randbuffer[0],&intstring[0], &intstring[0] + SHORTN);

  timeElapsed( &rt, &ct);

  printf("(short string) Multilinear usec %d  %d and sum is %d\n", rt, ct, sumToFoolCompiler);
  printf("Per character, clocks required is %g\n", ((float) rt)*CLOCKRATE /(((float) SHORTN) * SHORTTRIALS ));

   timeReset();
  for(i=0; i < SHORTTRIALS; ++i)
    sumToFoolCompiler += hashThorup( &randbuffer[0],&intstring[0],&intstring[0] + SHORTN);
  timeElapsed( &rt, &ct);

  

  printf("(short string) Thorup usec %d  %d and sum is %d\n", rt, ct, sumToFoolCompiler);
  printf("Per character, clocks required is %g\n", ((float) rt)*CLOCKRATE /(((float) SHORTN) * SHORTTRIALS ));

  timeReset();
  for(i=0; i < SHORTTRIALS; ++i)
    sumToFoolCompiler += hashSM2b2(  &randbuffer[0],&intstring[0],&intstring[0] + SHORTN);
  timeElapsed( &rt, &ct);

  

  printf("(short string) Multilinear 2-by-2 usec %d  %d and sum is %d\n", rt, ct, sumToFoolCompiler);
  printf("Per character, clocks required is %g\n", ((float) rt)*CLOCKRATE /(((float) SHORTN) * SHORTTRIALS ));

  timeReset();
  for(i=0; i < SHORTTRIALS; ++i)
    sumToFoolCompiler += hashMultilinearHalfMultiplications(  &randbuffer[0],&intstring[0],&intstring[0] + SHORTN);
  timeElapsed( &rt, &ct);

  printf("(short string) MultilinearHM usec %d  %d and sum is %d\n", rt, ct, sumToFoolCompiler);
  printf("Per character, clocks required is %g\n", ((float) rt)*CLOCKRATE /(((float) SHORTN) * SHORTTRIALS ));

  timeReset();
  for(i=0; i < SHORTTRIALS; ++i)
    sumToFoolCompiler += hashBerstein(  &intstring[0],&intstring[0] + SHORTN);
  timeElapsed( &rt, &ct);

  

  printf("(short string) Berstein usec %d  %d and sum is %d\n", rt, ct, sumToFoolCompiler);  
  printf("Per character, clocks required is %g\n", ((float) rt)*CLOCKRATE /(((float) SHORTN) * SHORTTRIALS ));

  timeReset();
  for(i=0; i < SHORTTRIALS; ++i)
    sumToFoolCompiler += hashSAX(  &intstring[0],&intstring[0] + SHORTN);
  timeElapsed( &rt, &ct);
  

  printf("(short string) SAX usec %d  %d and sum is %d\n", rt, ct, sumToFoolCompiler);    
  printf("Per character, clocks required is %g\n", ((float) rt)*CLOCKRATE /(((float) SHORTN) * SHORTTRIALS ));
  
  timeReset();

  for(i=0; i < SHORTTRIALS; ++i)
    sumToFoolCompiler += hashRabinKarp( &intstring[0],&intstring[0] + SHORTN);

  timeElapsed( &rt, &ct);

  printf("(short string) Rabin/Karp usec %d  %d and sum is %d\n", rt, ct, sumToFoolCompiler);  
  printf("Per character, clocks required is %g\n", ((float) rt)*CLOCKRATE /(((float) SHORTN) * SHORTTRIALS ));

  }

  exit(0);

}
