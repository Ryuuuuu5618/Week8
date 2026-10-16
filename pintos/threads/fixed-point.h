#include <stdint.h>

#define SCALE (1<<14)

/**
 * For 17.14 Fixed Point Operation
 */

typedef int32_t fixed_t;

static inline fixed_t int_to_fixed(int);
static inline int fixed_to_int(fixed_t);

static inline fixed_t fixed_mul(fixed_t, fixed_t);
static inline fixed_t fixed_div(fixed_t, fixed_t);
static inline fixed_t int_div(int, int);

static inline fixed_t round_nearest(fixed_t x){
	if(!(x&(0b1u<<31))){
		return x&(0b1u<<13);
	}else{
		return (~(uint32_t)((~(uint32_t)x+0b1u)&(0b1u<<13)))+0b1u;
	}
}

static inline fixed_t int_to_fixed(int n){
	return n * SCALE;
}
static inline int fixed_to_int(fixed_t x){
	return (x + round_nearest(x)) / SCALE;
}

static inline fixed_t fixed_mul(fixed_t x, fixed_t y){
	return((int64_t)x * y)/SCALE;
}
static inline fixed_t fixed_div(fixed_t x, fixed_t y){
	return ((int64_t)x*SCALE)/y;
}       
static inline fixed_t int_div(int a, int b){
	return (fixed_t)((int64_t)a*SCALE/b);
};