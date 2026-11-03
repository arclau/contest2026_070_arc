#!/usr/bin/env python3
"""把壁纸 PNG 编成 C 数组，随固件一起加载。

为什么不用 /resource 里的文件：板端 romfs 读路径会偶发返回坏字节（P312/P355），
1.6MB 的壁纸几乎每次读都坏，解码失败 → 每帧重试 → 1.3s 卡顿（P348）。
开机读一次也没用：实测 4 次重读全失败（P359 板上验证）。
放进 .rodata 后由 U-Boot 一次性载入内存，运行时不再碰 NAND。

用法：
    python3 tools/gen_wallpaper_blob.py \
        ../../../lichee/board/r528s3/ynm3000_nand/data/res/wallpaper/blue-hour.png \
        wallpaper_blob.c
"""
import sys

def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()

    with open(dst, "w", encoding="utf-8") as f:
        f.write("/* 自动生成，勿手改：%s\n" % src.split("/")[-1])
        f.write(" * python3 tools/gen_wallpaper_blob.py <png> wallpaper_blob.c */\n")
        f.write("#include <stddef.h>\n\n")
        f.write("const unsigned char dm_wallpaper_png[] =\n")
        # 八进制转义固定三位，不会像 \\x 那样吞掉后面的字符
        for i in range(0, len(data), 24):
            chunk = data[i:i + 24]
            f.write('    "' + "".join("\\%03o" % b for b in chunk) + '"\n')
        f.write(";\n\n")
        f.write("const size_t dm_wallpaper_png_len = sizeof(dm_wallpaper_png);\n")
    print("%s -> %s (%d bytes)" % (src, dst, len(data)))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
