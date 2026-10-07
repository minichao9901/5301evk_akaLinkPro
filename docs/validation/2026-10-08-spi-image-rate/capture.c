#include <stdint.h>
#define R(a) (*(volatile uint32_t *)(a))
__attribute__((naked,section(".entry"))) void entry(void) {
 __asm volatile("ldr sp, =0x20010000\n b main");
}
int main(void) {
 R(0x40021018)|=4;
 R(0x40013000)=0;
 R(0x40010800)=(R(0x40010800)&0x0000ffff)|0x44440000;
 R(0x20000804)=0; R(0x20000800)=1;
 volatile uint8_t *out=(void*)0x20001000;
 for (uint32_t n=0;n<8192;n++) {
  uint8_t b=0;
  for (unsigned k=0;k<8;k++) {
   while (!(R(0x40010808)&32)) {}
   b=(b<<1)|((R(0x40010808)>>7)&1);
   while (R(0x40010808)&32) {}
  }
  out[n]=b; R(0x20000804)=n+1;
 }
 for (;;) {}
}
