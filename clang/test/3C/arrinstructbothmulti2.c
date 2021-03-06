// RUN: 3c -base-dir=%S -addcr -alltypes -output-postfix=checkedALL2 %S/arrinstructbothmulti1.c %s
// RUN: 3c -base-dir=%S -addcr -output-postfix=checkedNOALL2 %S/arrinstructbothmulti1.c %s
// RUN: %clang -c %S/arrinstructbothmulti1.checkedNOALL2.c %S/arrinstructbothmulti2.checkedNOALL2.c
// RUN: FileCheck -match-full-lines -check-prefixes="CHECK_NOALL","CHECK" --input-file %S/arrinstructbothmulti2.checkedNOALL2.c %s
// RUN: FileCheck -match-full-lines -check-prefixes="CHECK_ALL","CHECK" --input-file %S/arrinstructbothmulti2.checkedALL2.c %s
// RUN: 3c -base-dir=%S -alltypes -output-postfix=checked2 %S/arrinstructbothmulti1.c %s
// RUN: 3c -base-dir=%S -alltypes -output-postfix=convert_again %S/arrinstructbothmulti1.checked2.c %S/arrinstructbothmulti2.checked2.c
// RUN: test ! -f %S/arrinstructbothmulti1.checked2.convert_again.c
// RUN: test ! -f %S/arrinstructbothmulti2.checked2.convert_again.c
// RUN: rm %S/arrinstructbothmulti1.checkedALL2.c %S/arrinstructbothmulti2.checkedALL2.c
// RUN: rm %S/arrinstructbothmulti1.checkedNOALL2.c %S/arrinstructbothmulti2.checkedNOALL2.c
// RUN: rm %S/arrinstructbothmulti1.checked2.c %S/arrinstructbothmulti2.checked2.c


/*********************************************************************************/

/*This file tests three functions: two callers bar and foo, and a callee sus*/
/*In particular, this file tests: how the tool behaves when there is an array
field within a struct*/
/*For robustness, this test is identical to arrinstructprotoboth.c and arrinstructboth.c except in that
the callee and callers are split amongst two files to see how
the tool performs conversions*/
/*In this test, foo will treat its return value safely, but sus and bar will not,
through invalid pointer arithmetic, an unsafe cast, etc.*/

/*********************************************************************************/


#include <stddef.h>
extern _Itype_for_any(T) void *calloc(size_t nmemb, size_t size) : itype(_Array_ptr<T>) byte_count(nmemb * size);
extern _Itype_for_any(T) void free(void *pointer : itype(_Array_ptr<T>) byte_count(0));
extern _Itype_for_any(T) void *malloc(size_t size) : itype(_Array_ptr<T>) byte_count(size);
extern _Itype_for_any(T) void *realloc(void *pointer : itype(_Array_ptr<T>) byte_count(1), size_t size) : itype(_Array_ptr<T>) byte_count(size);
extern int printf(const char * restrict format : itype(restrict _Nt_array_ptr<const char>), ...);
extern _Unchecked char *strcpy(char * restrict dest, const char * restrict src : itype(restrict _Nt_array_ptr<const char>));

struct general { 
    int data; 
    struct general *next;
	//CHECK: _Ptr<struct general> next;
};

struct warr { 
    int data1[5];
	//CHECK_NOALL: int data1[5];
	//CHECK_ALL: int data1 _Checked[5];
    char *name;
	//CHECK: _Ptr<char> name;
};

struct fptrarr { 
    int *values; 
	//CHECK: _Ptr<int> values; 
    char *name;
	//CHECK: _Ptr<char> name;
    int (*mapper)(int);
	//CHECK: _Ptr<int (int )> mapper;
};

struct fptr { 
    int *value; 
	//CHECK: _Ptr<int> value; 
    int (*func)(int);
	//CHECK: _Ptr<int (int )> func;
};  

struct arrfptr { 
    int args[5]; 
	//CHECK_NOALL: int args[5]; 
	//CHECK_ALL: int args _Checked[5]; 
    int (*funcs[5]) (int);
	//CHECK_NOALL: int (*funcs[5]) (int);
	//CHECK_ALL: _Ptr<int (int )> funcs _Checked[5];
};

static int add1(int x) { 
	//CHECK: static int add1(int x) _Checked { 
    return x+1;
} 

static int sub1(int x) { 
	//CHECK: static int sub1(int x) _Checked { 
    return x-1; 
} 

static int fact(int n) { 
	//CHECK: static int fact(int n) _Checked { 
    if(n==0) { 
        return 1;
    } 
    return n*fact(n-1);
} 

static int fib(int n) { 
	//CHECK: static int fib(int n) _Checked { 
    if(n==0) { return 0; } 
    if(n==1) { return 1; } 
    return fib(n-1) + fib(n-2);
} 

static int zerohuh(int n) { 
	//CHECK: static int zerohuh(int n) _Checked { 
    return !n;
}

static int *mul2(int *x) { 
	//CHECK: static _Ptr<int> mul2(_Ptr<int> x) _Checked { 
    *x *= 2; 
    return x;
}

struct warr * sus(struct warr * x, struct warr * y) {
	//CHECK_NOALL: struct warr * sus(struct warr * x, struct warr * y) {
	//CHECK_ALL: _Array_ptr<struct warr> sus(struct warr * x, struct warr *y : itype(_Array_ptr<struct warr>)) {
x = (struct warr *) 5;
	//CHECK: x = (struct warr *) 5;
        char name[20]; 
	//CHECK_NOALL: char name[20]; 
	//CHECK_ALL: char name _Checked[20]; 
        struct warr *z = y;
	//CHECK_NOALL: struct warr *z = y;
	//CHECK_ALL: _Array_ptr<struct warr> z = y;
        int i;
        for(i = 0; i < 5; i++) { 
	//CHECK_NOALL: for(i = 0; i < 5; i++) { 
	//CHECK_ALL: for(i = 0; i < 5; i++) _Checked { 
            z->data1[i] = i; 
        }
        
z += 2;
return z; }
