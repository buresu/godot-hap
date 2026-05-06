# godot-hap
Godot HAP Addon via GDExtension

## HAP Q
If you are using HAP Q, you will need to decode YCoCg.  
Please set the ycocg.gdshader included with the add-on as the material for your VideoStreamPlayer.  

## Convert HAP with FFmpeg
FFmpeg is a convenient tool for converting videos to HAP format.  
```
ffmpeg -i input.mp4 -vcodec hap -format [hap|hap_alpha|hap_q] output.mov
```

## Build
```
git clone --recursive https://github.com/buresu/godot-hap.git
cd godot-hap
mkdir build && cd build

[Windows]
cmake -G "Visual Studio 18 2026" -A [x64|Win32] ..
cmake --build . --config [Debug|Release] --target install

[Mac, Linux]
cmake -DCMAKE_BUILD_TYPE=[Debug] ..
cmale --build . --target install
```

## License
MIT License
