
To build, you need something like this to be installed:
```
vulkan (sdk)
ffmpeg
sdl3 (sdk)
```


To stop rendring, create file `stop_now`
```powershell
"">stop_now
```


To see video use
```powershell
ffplay result.mkv
```


To convert video to common format, use
```
ffmpeg -i input.mkv -c:v libx264 -crf 17 -pix_fmt yuv420p -c:a copy output.mkv
```
or
```
ffmpeg -i input.mkv -c:v libx264 -crf 20 -pix_fmt yuv420p -c:a copy output.mkv
```
or anything what you want.



```
points:

-c -0.743643887037158 0.131825904205311                                        #      65
-c -1.749759145120931 0.000000003906887                                        #      70
-c -0.0864380753683 0.6552945657805                                            #      30
-c -1.250669010848506198642137 0.020120338450174828114402                      #      95
-c -1.9999858817081772 1e-100                                                  #      60
-c -0.77659190638318281232822 0.13664081033010313886566                        #      70
-c -0.749127680229678085786825026861 0.053200536224596671558212847644          #      60
-c -1.543689 1e-100                                                            #      60
```
