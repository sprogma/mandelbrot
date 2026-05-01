$DEBUG = @("-g", "-fsanitize=address")

$CFLAGS = @("-std=gnu2y") + $DEBUG
$LDFLAGS = @("-lsdl3", "-LD:/c/SDL3/lib/x64", "-LD:/c/vulkanSDK/Lib", "-lvulkan-1") + $DEBUG

clang (ls *.c | % { 
    (clang -c $_ -o "$_.o" $CFLAGS | oh) || $(Write-Host "Error: file $($_.Name) con't compiled" -Fore red; exit 1)
    "$_.o"
}) -o brainrot.exe $LDFLAGS || $(Write-Host "Error: code don't linked" -Fore red; exit 1)

$OPT = "-O3", "-all-resources-bound"

dxc  -Wall -Wextra -spirv -T cs_6_0 -E main $OPT kernel.hlsl -Fo kernel.spv || $(Write-Host "Error: Shader don't compiled" -Fore red; exit 1)
spirv-opt -O kernel.spv -o kernel_opt.spv
