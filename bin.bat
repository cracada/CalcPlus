@windres app.rc -o app_res.o
REM gcc CalcPlus.c app_res.o -o CalcPlus.exe -O2 -s -municode -ladvapi32 -lgdi32 -lrpcrt4
REM cl /utf-8 /DUNICODE /D_UNICODE CalcPlus.cpp /link Rpcrt4.lib Advapi32.lib User32.lib Gdi32.lib
gcc -municode -std=c11 -O2 ^
    -DUNICODE -D_UNICODE ^
    -finput-charset=UTF-8 ^
    -fexec-charset=UTF-8 ^
    -fwide-exec-charset=UTF-16LE ^
    CalcPlus.c app_res.o -o CalcPlus.exe ^
    -lrpcrt4 -ladvapi32 -luser32 -lgdi32 ^
    -static-libgcc -s