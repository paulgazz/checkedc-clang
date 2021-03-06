// RUN: 3c -alltypes -addcr %s -- | FileCheck -match-full-lines -check-prefixes="CHECK_ALL","CHECK" %s
// RUN: 3c -addcr %s -- | FileCheck -match-full-lines -check-prefixes="CHECK_NOALL","CHECK" %s
// RUN: 3c -addcr %s -- | %clang -c -fcheckedc-extension -x c -o /dev/null -
// RUN: 3c -output-postfix=checked -alltypes %s
// RUN: 3c -alltypes %S/ptr_array.checked.c -- | count 0
// RUN: rm %S/ptr_array.checked.c

/* Tests for issue 60. Array initialization had not been implemented, so wild
 pointer inside and array initializer did not cause the array to be an array
 of wild pointer. */
	//CHECK: of wild pointer. */

/* A checked pointer should allow the array to be checked. */
void test0(int *a) {
	//CHECK_NOALL: void test0(int *a) {
	//CHECK_ALL: void test0(_Ptr<int> a) _Checked {

  int *b[1] = {a};
	//CHECK_NOALL: int *b[1] = {a};
	//CHECK_ALL:   _Ptr<int> b _Checked[1] =  {a};
}

/* An unchecked pointer should cause the array to be unchecked. */
void test1(int *a) {
	//CHECK: void test1(int *a) {

  a = (int*) 4;
	//CHECK: a = (int*) 4;

  int *b[1] = {a};
	//CHECK_NOALL: int *b[1] = {a};
	//CHECK_ALL:   int * b _Checked[1] =  {a};
}

/* Example from from the issue */
int *foo() {
	//CHECK: int *foo(void) {

  int x = 1;
  int y = 2;
  int z = 3;
  int *ptrs[4] = { &x, &y, &z, (int *)5 };
	//CHECK_NOALL: int *ptrs[4] = { &x, &y, &z, (int *)5 };
	//CHECK_ALL:   int * ptrs _Checked[4] =  { &x, &y, &z, (int *)5 };
  int *ret;
	//CHECK: int *ret;
  for (int i = 0; i < 4; i++) {
    ret = ptrs[i];
  }

  return ret;
}

/* Example from the issue, but everthing should check */
int *foo2() {
	//CHECK_NOALL: int *foo2(void) {
	//CHECK_ALL: _Ptr<int> foo2(void) _Checked {

  int x = 1;
  int y = 2;
  int z = 3;
  int *ptrs[4] = { &x, &y, &z, &x};
	//CHECK_NOALL: int *ptrs[4] = { &x, &y, &z, &x};
	//CHECK_ALL:   _Ptr<int> ptrs _Checked[4] =  { &x, &y, &z, &x};
  int *ret;
	//CHECK_NOALL: int *ret;
	//CHECK_ALL:   _Ptr<int> ret = ((void *)0);
  for (int i = 0; i < 4; i++) {
    ret = ptrs[i];
  }

  return ret;
}
