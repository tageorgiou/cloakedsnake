/* get, build, install MPFQ library from INRIA */

// gcc owenmpfq.c -O3 -lgmp -lmpfq_gf2n
// old: runs in 7.69s on Owen's machine
// new: runs in 9.6s on Owen's machine zeal

#include "mpfq/mpfq_2_32.h"
#include "stdlib.h"

//hashfunctionsreal uses N=1024 T=1000000
#define N 1024   
#define T 1000001

int main() {

  int i,j,k;
  mpfq_2_32_elt m[N], s[N];
  // unsigned long thirtytwo = 32;
 
  //int acc=0;
  unsigned long fake_dep=0;

  mpfq_2_32_field gf232;

  //return foo(); // temp

  mpfq_2_32_field_init(gf232);
  //mpfq_2_32_field_specify(gf232,MPFQ_DEGREE,&thirtytwo);  // does not help


  for (i=0; i < N; ++i) {
    mpfq_2_32_init(gf232, &m[i]);
    // mpfq_2_32_set_ui(gf232, m[i], (((unsigned long)rand())<<32) + rand() );
    // mpfq_2_32_set_ui(gf232, m[i], 6UL); 
    // the _ui stuff apparently only works for prime fields (?) from
    // reading the perl interface source.  Big problem for me...

    // however, for GF[2**32] I think we can just break the abstraction.

    mpfq_2_32_random(gf232,m[i]);  // works 
    /* m[i] = rand(); */

    mpfq_2_32_init(gf232, &s[i]);
    mpfq_2_32_random(gf232,s[i]); 
    /* s[i] = (unsigned long) rand(); */
  }

  // mpfq_2_32_sscan(gf232, m[0], "6");  // works

  
  // printf("%d\n",(int) mpfq_2_32_get_ui(gf232,m[0])); answer in GF(2)....
  
  //printf("now use their print:");
  //mpfq_2_32_print(gf232, m[0]);
  //printf("\n");


  for (i=0; i < T; ++i) {
    // int res = m[0];
    mpfq_2_32_elt res;
    mpfq_2_32_elt temp_sum_1, temp_sum_2, temp_prod;
    mpfq_2_32_init(gf232,&res);
    mpfq_2_32_init(gf232,temp_sum_1);
    mpfq_2_32_init(gf232,temp_sum_2);
    mpfq_2_32_init(gf232,temp_prod);
    

    // accumulate  (m[2j]+s[2j-1])*(m[2J+1]s[2j])
    for (j=0; j < N/2; ++j) {
      // loop body has about 100 machine instructions
      mpfq_2_32_add(gf232, temp_sum_1, m[2*j], s[2*j-1]);
      mpfq_2_32_add(gf232, temp_sum_2, m[2*j+1], s[2*j]);
      mpfq_2_32_mul(gf232, temp_prod, temp_sum_1, temp_sum_2);
      mpfq_2_32_add(gf232, res, res, temp_prod);
    }
    fake_dep ^= mpfq_2_32_get_ui(gf232, res);  // getUI returns nothing useful
  }
  printf("fake dep returns %ld\n",fake_dep);
  printf("Finished test with %d trials over strings of length %d\n",T,N);


   
}
