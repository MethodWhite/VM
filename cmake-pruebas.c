
#ifndef __VPP_VERSION__
#include <stdio.h>
#endif

int main()
{
#ifdef __VPP_VERSION__
#   warning "Esta usando el preproceesador de VPP"
#   array OPCODES(ADD, SUB, MUL, DIV)

#   foreach OP in OPCODES
        puts(__TOLOWER__(OP));
#   endforeach
#else
#endif

    return 0;
}