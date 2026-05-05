
# $DEBUG = @("-g", "-fsanitize=address")

$CFLAGS = @("-std=gnu2y",
            "-O3",
            "-march=native"
            # "-Rpass=loop-vectorize",
            # "-Rpass-missed=loop-vectorize",
            # "-Rpass-analysis=loop-vectorize"
            ) + $DEBUG
$LDFLAGS = @("-lsdl3", "-LD:/c/SDL3/lib/x64", 
             "-LD:/c/vulkanSDK/Lib", "-lvulkan-1",
             "-LC:/ffmpeg/lib",
             "-LC:/Program Files/ffmpeg/lib", "-lavcodec",
             "-lavutil",
             "-lswscale",
             "-lavformat") + $DEBUG

clang (ls *.c -Exclude lli_test.c | % { 
    (clang -c $_ -o "$_.o" $CFLAGS | oh) || $(Write-Host "Error: file $($_.Name) con't compiled" -Fore red; exit 1)
    "$_.o"
}) -o frac.exe $LDFLAGS || $(Write-Host "Error: code don't linked" -Fore red; exit 1)

clang lli_test.c -o test.exe ($CFLAGS|?{$_ -notmatch "-O\d"}) || $(Write-Host "Error: tests don't compiled" -Fore red; exit 1)
./test.exe || $(Write-Host "Error: tests failed" -Fore red; exit 1)

$OPT = "-O3", "-all-resources-bound"

dxc  -Wall -Wextra -spirv -T cs_6_0 -E main $OPT kernel.hlsl -Fo kernel.spv || $(Write-Host "Error: Shader don't compiled" -Fore red; exit 1)
spirv-opt -O kernel.spv -o kernel_opt.spv
