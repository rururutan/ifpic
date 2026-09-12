# Susie64 Yanagisawa PIC plugin

This software is a Susie x64 plugin for Yanagisawa PIC format images.

Support extended specifications for PC-88VA, FM TOWNS, and Macintosh in addition to the standard X68000 specification.

## Build

Open `src/ifpic.sln` in Visual Studio 2022 and build the `Release|x64`
configuration. The output file is `x64/Release/ifpic.sph`.

## Thanks

The Susie I/F layer code was based on Masaru Miyasaka's Susie32 Pi Plug-in.
The PIC decoding specification was implemented with reference to [PIC_FMT](https://www.vector.co.jp/soft/data/art/se003198.html) and [PIC extend header](http://retropc.net/x68000/software/graphics/pic/picheader.htm).

## License

MIT
