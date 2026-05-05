



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

