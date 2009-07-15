// RUN: clang-cc -fsyntax-only -verify %s

class Base { // expected-error {{cannot define the implicit default assignment operator for 'class Base'}} \
             // expected-note {{synthesized method is first required here}}
  int &ref;  // expected-note {{declared at}}
};

class X  : Base {  // // expected-error {{cannot define the implicit default assignment operator for 'class X'}}
public:
  X();
  const int cint;  // expected-note {{declared at}}
}; 

struct Y  : X { 
  Y();
  Y& operator=(const Y&);
  Y& operator=(volatile Y&);
  Y& operator=(const volatile Y&);
  Y& operator=(Y&);
}; 

class Z : Y {};

Z z1;
Z z2;

// Test1
void f(X x, const X cx) {
  x = cx;	// expected-note {{synthesized method is first required here}}
  x = cx;
  z1 = z2;
}

// Test2
class T {};
T t1;
T t2;

void g()
{
  t1 = t2;
}

// Test3
class V {
public:
  V();
  V &operator = (V &b);
};

class W : V {};
W w1, w2;

void h()
{
  w1 = w2;
}

// Test4

class B1 {
public:
  B1();
  B1 &operator = (B1 b);
};

class D1 : B1 {};
D1 d1, d2;

void i()
{
	d1 = d2;
}
