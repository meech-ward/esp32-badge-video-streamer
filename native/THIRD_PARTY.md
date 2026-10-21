# Native dependency provenance and notices

See [README.md](README.md) for building. This project's MIT license does not
replace the licenses of linked dependencies. Retain their notices when sharing
source or binaries, including the SDK's submodule and CYW43 firmware licenses.

| Component | Tested source | License |
| --- | --- | --- |
| Pico SDK | [2.2.0, `a1438dff1d38bd9c65dbd693f0e5db4b9ae91779`](https://github.com/raspberrypi/pico-sdk/tree/a1438dff1d38bd9c65dbd693f0e5db4b9ae91779), with submodules | [BSD-3-Clause](https://github.com/raspberrypi/pico-sdk/blob/a1438dff1d38bd9c65dbd693f0e5db4b9ae91779/LICENSE.TXT); submodules carry their own licenses |
| Display setup/PIO | [Pimoroni badgeware-cpp `7d1ef0882c6e9dcf374542528f6f211e287b5afb`](https://github.com/pimoroni/badgeware-cpp/tree/7d1ef0882c6e9dcf374542528f6f211e287b5afb) | MIT notice below |
| Tufty board header | Raspberry Pi board definition adapted for Tufty 2350 | BSD-3-Clause notice below |
| PSRAM setup | [Pico SDK `hardware_psram` at `98a542c1a62fb549ffb5d66a3e5892b06276b670`](https://github.com/raspberrypi/pico-sdk/tree/98a542c1a62fb549ffb5d66a3e5892b06276b670/src/rp2_common/hardware_psram) | BSD-3-Clause notice below; boot-only direct probe supports SDK 2.2.0 |
| JPEGDEC | [`lib/jpegdec` in Pimoroni PicoVector `9fdf8e62bcca93f8d53f8ba9278d667bc6a058dd`](https://github.com/pimoroni/picovector/tree/9fdf8e62bcca93f8d53f8ba9278d667bc6a058dd/lib/jpegdec) | [Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0); Copyright 2020 BitBank Software, Inc. |
| CYW43 driver | [SDK submodule `dd7568229f3bf7a37737b9e1ef250c26efe75b23`](https://github.com/georgerobotics/cyw43-driver/tree/dd7568229f3bf7a37737b9e1ef250c26efe75b23) | Raspberry Pi semiconductor license below |

JPEGDEC is a directory in the PicoVector Git repository, not a separate Git
checkout. `JPEGDEC_PATH` must contain these exact tested files (SHA-256):

```text
JPEGDEC.cpp  4d342e77527c4a170cc4961d23bb0d1830211175d645efd77d190a96d5590196
JPEGDEC.h    fa54da60a78ed7805b7faf6e1dcdf7496ad71261ddabb1e7acefe3eed63637b6
jpeg.inl     69e6c376572e7f849b7e17d7b6ab6df31d6adb7090bc3767ca4c14a0fe275630
```

Optional [prepare_jpegdec.py](prepare_jpegdec.py) stages build-local copies.
`--dsp` fixes two coefficient and three pixel lane-packing expressions before
the existing DSP path is enabled by [jpeg_arm_dsp.hpp](jpeg_arm_dsp.hpp).
`--sram` adds placement attributes to selected hot functions/tables; it does not
change conversion arithmetic. Host tests compare full-frame color hashes across
scalar, DSP and SRAM profiles. Original dependency files and notices are retained.

[prepare_cyw43_tx.py](prepare_cyw43_tx.py) likewise stages `cyw43_ll.c`, requiring
SHA-256 `7f4c93755b2ab911d18606652497b2f3cd08085ce63b3b1836171d375f806b10`.
It bounds Ethernet data flow-control waits to 8 ms, retaining control timeouts.
The SDK checkout is unchanged. CYW43 uses these Raspberry Pi semiconductor terms:

Copyright (C) 2019-2022 George Robotics Pty Ltd

Raspberry Pi Ltd (Licensor) hereby grants to you a non-exclusive license to
use this software solely with the Licensor's microcontroller chip (RP2040) or
any other semiconductor device produced by the Licensor. No other use is
permitted under the terms of this licence.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. The software can only be used and redistributed in conjunction with RP2040
   or any other semiconductor device produced by the Licensor.
2. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
3. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE LICENSOR AND COPYRIGHT OWNER "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE LICENSOR OR COPYRIGHT OWNER BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Pimoroni's MIT notice applies to the adapted display code:

Copyright (c) 2026 Pimoroni Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

The SDK, adapted board header and PSRAM setup carry these BSD-3-Clause notices:

Copyright 2020 (c) 2020 Raspberry Pi (Trading) Ltd.

Copyright (c) 2024 Raspberry Pi (Trading) Ltd.

Copyright (c) 2026 Raspberry Pi (Trading) Ltd.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
