$FLAGS = @("-std=gnu2y")
$LDFLAGS = @("-lsdl3", "-LD:/c/SDL3/lib/x64", "-LD:/c/vulkanSDK/Lib", "-lvulkan-1")

clang (ls *.c | % { clang -c $_ -o "$_.o" $FLAGS | oh; "$_.o"}) -o brainrot.exe $LDFLAGS
