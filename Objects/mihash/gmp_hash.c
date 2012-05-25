
/*

     USING 3.5.2

 gcc -O3 -DBITS_IN=n gmp_hash.c  -lgmp
 runs in 37.9s on Owen's machine, with 32 bits in, 32 bits out
  (and no faster when the div and mod are removed)
 in 34s with 16 bits in and 16 bits out.
 in 96s    with 256 bits in and 32 bits out.
 in 366s   with 1024 bits in and 32 bits out.

 Redone so that the same amount (bits) of input data is processed as with 32 bits:
 Real times on Owen's machine zeal (always 32 bits out):
  32  in 48.5s 48.3s 49.8s avg 48.9
  64  in 37.3s 37.4s 37.4s avg 37.4
  128 in 21.8s 22.0s 22.1s avg 22.0
  256 in 14.9s 14.7s 14.8s avg 14.8
  512 in  12.9 12.9s 13.0s avg 12.9
  1024 in 15.3s 15.3s 15.3 avg 15.3
  2048 in 22.3s 22.3s 22.3savg 22.3

After minor bugfix that should not have affected time by much: 
32 -> 46.5; 46.6; 46.6
64 -> 39.1; i

 Assembly code shows it's all done via function calls (not inlined)
 C++ version of this code was the same way
*/


#include <stdlib.h>
#include <stdio.h>
#include <gmp.h>
//#include "/home/owen/gmp-5.0.2/gmp.h"


#define N 1024
#define T 1000001

#ifndef BITS_IN 
 #define BITS_IN 32
#endif

#define BITS_OUT 32
#define K (BITS_IN + BITS_OUT)

int main() {

  int i,j,k;
  mpz_t m[N+1], s[N];
  int n1 = (N * 32)/BITS_IN;
  gmp_randstate_t state;

  unsigned long fake_dep=0;
  
  gmp_randinit_mt(state);

  for (i=0; i < n1; ++i) {
    mpz_init(m[i]);
    mpz_urandomb(m[i],state,K);

    mpz_init(s[i]);
    mpz_urandomb(s[i],state,BITS_IN );
  }

  for (i=0; i < T; ++i) {
    mpz_t res;
    mpz_t temp_sum_1, temp_sum_2, temp_prod;

    mpz_init_set(res,m[0]);

    mpz_init(temp_sum_1);
    mpz_init(temp_sum_2);
    mpz_init(temp_prod);

    
    for (j=0; j < n1 /2; ++j) {
      mpz_add(temp_sum_1, m[2*j], s[2*j-1]);
      mpz_add(temp_sum_2, m[2*j+1], s[2*j]);
      mpz_addmul(res, temp_sum_1, temp_sum_2);
    }
    mpz_tdiv_r_2exp(res, res, K);  /* mod */
    mpz_tdiv_q_2exp(res, res, BITS_IN); /* div */

    fake_dep ^= mpz_get_ui(res);

    //    printf("hashed to %ld\n", mpz_get_ui(res));

  }
  printf("fake dep returns %ld\n",fake_dep);
  exit(0);
}
