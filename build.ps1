if (!$CLANG) $CLANG = "clang"

$DEBUG = @("-g")
# $DEBUG = @("-g", "-fsanitize=address")

$CFLAGS = @("-std=gnu2y",
            "-O3",
            "-march=native",
            "-fms-extensions"
            "-Wno-microsoft"
            # "-Rpass=loop-vectorize",
            # "-Rpass-missed=loop-vectorize",
            # "-Rpass-analysis=loop-vectorize"
            ) + $DEBUG

if ($IsLinux)
{
    $CFLAGS += "-Wno-format"
}

if ($IsWindows)
{
    $LDFLAGS = @("-lsdl3", "-LD:/c/SDL3/lib/x64", 
                 "-LD:/c/vulkanSDK/Lib", "-lvulkan-1",
                 "-LC:/ffmpeg/lib",
                 "-LC:/Program Files/ffmpeg/lib", "-lavcodec",
                 "-lavutil",
                 "-lswscale",
                 "-lavformat") + $DEBUG
}
else
{
    $LDFLAGS = -split"$(pkg-config --libs sdl3 vulkan libavcodec libavutil libswscale libavformat) -lm" + $DEBUG
}

& $CLANG (ls *.c -Exclude lli_test.c | % { 
    (& $CLANG -c $_ -o "$_.o" $CFLAGS | oh) || $(Write-Host "Error: file $($_.Name) con't compiled" -Fore red; exit 1)
    "$_.o"
}) -o frac.exe $LDFLAGS || $(Write-Host "Error: code don't linked" -Fore red; exit 1)

& $CLANG lli_test.c -o test.exe -lm ($CFLAGS|?{$_ -notmatch "-O\d"}) || $(Write-Host "Error: tests don't compiled" -Fore red; exit 1)
./test.exe || $(Write-Host "Error: tests failed" -Fore red; exit 1)

$OPT = "-O3", "-all-resources-bound"

if ($IsLinux)
{
    Write-Host "Waring: building of shaders may be failed. Use precompiled then" -Fore yellow
}
try
{
    dxc  -Wall -Wextra -spirv -T cs_6_0 -E main $OPT kernel.hlsl -Fo kernel.spv || $(Write-Host "Error: Shader don't compiled" -Fore red; throw)
    spirv-opt -O kernel.spv -o kernel_opt.spv

    dxc -DFLOAT64 -Wall -Wextra -spirv -T cs_6_0 "-fspv-target-env=vulkan1.1" -E main $OPT kernel.hlsl -Fo kernel64.spv || $(Write-Host "Error: Shader 64bit don't compiled" -Fore red; throw)
    spirv-opt -O kernel64.spv -o kernel64_opt.spv

    dxc -DFLOATFLOAT -Wall -Wextra -spirv -T cs_6_0 "-fspv-target-env=vulkan1.1" -E main $OPT kernel.hlsl -Fo kernelff.spv || $(Write-Host "Error: Shader float-float don't compiled" -Fore red; throw)
    spirv-opt -O kernelff.spv -o kernelff_opt.spv
} 
catch 
{
    Write-Host "Waring: Using precompiled shaders" -Fore yellow
    cp spv/*
}
