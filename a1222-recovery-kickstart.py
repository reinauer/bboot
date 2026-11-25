#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Make the A1222 Kickstart zip archive from the A1222 recovery image.

  a1222-recovery-kickstart.py [-o Kickstart-a1222.zip] RECOVERY-IMAGE

The AmigaOS 4.1 recovery image for the AmigaOne A1222,
A1222-Emergency-4G-AAATech-FW8-2024-12-28.img (MD5
b43d29cd1c84ac13028be389f6495c80), keeps its Kickstart files on an NGFS
partition, which no tool outside AmigaOS reads.  This script copies them
from where they are in that particular image, checks each one against
its SHA-256, and makes the zip archive that BBoot boots from, with
mkkickzip.py.  It works with that image only.

The table holds positions and checksums, none of the files: the
Kickstart is not free, and you need your own recovery image.

RadeonRX.chip is fragmented in the image and not copied, so its line
in Kicklayout is commented out.  QEMU emulates no Radeon RX card anyway.
"""

import argparse
import hashlib
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkkickzip  # noqa: E402

IMAGE_NAME = 'A1222-Emergency-4G-AAATech-FW8-2024-12-28.img'
IMAGE_MD5 = 'b43d29cd1c84ac13028be389f6495c80'
IMAGE_SIZE = 4026531840

# name, offset in the image, size, SHA-256
FILES = [
    ("3DLabsPermedia2.chip",      1898502144,    27844, "d672bdd64161d900bf57a0c0034a8a6da0743a69f4eedd5734e6743cb5beb8fe"),
    ("3dfxVoodoo.chip",           1898401792,    96836, "8d8fe602ccd85578601f5cf1df145e6ababfb1da37c0112b1035cbdaca0071fd"),
    ("ATIRadeon.chip",            1898532864,   154716, "d39877aa4c632db740c410023820f6af11f1769a0263360342181092ba73b581"),
    ("CDFileSystem",              1898934272,    96612, "95d30bd264d566585f9afdd848a3bae325e92faddef67c1939c820773aae1af5"),
    ("FastFileSystem",            1900060672,   159852, "1e1617ff38d1889ba0c63f460c3f77bd87e90dc806d954696200ea7af7c02971"),
    ("FileSystem.resource.kmod",  1900224512,     4848, "166e894f9d6aed8df28c2aa0b476fba28fa3615a22cc2a533c596c3ff48d5975"),
    ("NGFileSystem",              1905649664,   200064, "bf4c1dc5e40632034bef82926f3bd463e66d931c43543292215890b20474bae9"),
    ("P1022DIU.chip",             1905936384,    18452, "1ace21b2113fc15104af4113e7966fbfb85162fc7b5f1aa5760536ee4fceb393"),
    ("PCIGraphics.card",          2637963264,    49868, "0c4328616b32fc8ff368e04a72792fd1b054306bc5507326e4d982b2d5a0d63a"),
    ("RadeonHD.chip",             1909057536,  1795884, "e9292cbf4726bc23d3cd5e6d1cf8aa3fad996de40a9b08eb66f45a73f359df31"),
    ("SmartFilesystem",           1671874255,   202980, "895de758d0a63a98985e98cc13ba3ad36c804ddee8480d6b90b512c91050e0ae"),
    ("battclock.resource.kmod",   1898694656,    20372, "e636cc6f16723f8df94b1c6e74cc0fca11aff3609e734cd7adf2d6046adc5c49"),
    ("bootimage",                 1898717184,    79632, "43d521678a522a619558b69ef05a02196b5d3dda9e26716163af122f145f8dd9"),
    ("bootkeyboard.usbfd",        1898799104,    17828, "0ae4d6e0e28ee0cadfb500e89c9e811ba08cde59dddc660bfce8700b02fe19a6"),
    ("bootmenu.kmod",             1898819584,    61124, "a05d1b3690c4b63341311f16469e576831c5ded344f442ad936846f3c6f9ec07"),
    ("bootmouse.usbfd",           1898883072,    10320, "f83d1c836bd2afd42ca9a4a0ebff971b860ef1ab6cca59da8ebe894481044c16"),
    ("bootsd.resource.kmod",      1898897408,    23612, "35fd1fdd3269ba48ef5a209a234184735804d6f9ef2bf17cd43fb345c4df2985"),
    ("con-handler.kmod",          1899034624,    77660, "0f77d379f6f8ade036fcf727fd91f686fb0668a0aaac3c9256dd2e7c969ba7ff"),
    ("console.device.kmod",       1899114496,   176644, "613ca2fa33ca0bdf4ac99a2db3063b7055816fc9b81cb32bc6fac48a079c6e89"),
    ("diskboot.config",           1899298816,      442, "051e6b1835e0e1fd0db9bcacafbc9e6e27af4590526ff47a4c8398dce93d4c92"),
    ("diskboot.kmod",             1899304960,    28272, "56cff7983af50d0223380d897f42b716f524d7bc369656491ac4831f2287fd18"),
    ("dos.library.kmod",          1899360256,   464620, "6889dc08b95926161a6cb4ee8ed200a49bc0bf8541e5a0d7c2e404633e603931"),
    ("ehci.usbhcd",               1899827200,   102048, "8355cb47bac58f93a888ff2967674728200a9593d54ab1fe60aa5e4cd81ce4bb"),
    ("elf.library.kmod",          1899931648,    98708, "348afe135691dd102116a89bc02f1fbad464398f15a532c37691c0babab4012b"),
    ("env-handler.kmod",          1900034048,    23804, "b76136d93cb6160071e4de29e5638a50f6964895a5d189e4e349c35859a5b88f"),
    ("gadtools.library.kmod",     1900234752,    64332, "94df76d346b0e3a1395d4202ccde6018976b027e37abfcfc588c9602020e8cde"),
    ("graphics.library.kmod",     2640824320,  1118056, "4004821011e430241a30d79df3b8b79651ee40a035b41a58f6a1aba63a528f9a"),
    ("hub.usbfd",                 1901430784,    71200, "4792a5e15a137d9d17d5bb027ab831728a2d88e679be22416d75388f90c581e8"),
    ("hunk.library.kmod",         1901504512,    67264, "acdca712b2987f475c731d2f3350e0d261f0666119c10acd2179d1364c49c4dd"),
    ("input.device.kmod",         1901574144,    14412, "b997d1591d83affdd6491720978d3012767c3e63a1ebc4cd70b2d705c9244562"),
    ("intuition.library.kmod",    1901592576,   835856, "7ac626ae4acfbcad4524ed24d034ef712873ef70fbeb6761ef4699a94da67b92"),
    ("kernel",                    1902508032,   807744, "8fe77f527ae79ded06d7f0d4299f9cd71440339b5bebabf9a271b351505c5326"),
    ("keyboard.device.kmod",      1904513024,    18000, "23e44f86593f547144ceecbed831bf3acc5cd67057fc613b955396b71a0fc6bb"),
    ("keymap.library.kmod",       1904533504,    68892, "2f7439114230d0ab006dfb418bb52941316af097601e2ea3e79c680385d64866"),
    ("loader",                    1904623616,    30888, "703fe3db6acae57edea0e08c0b989f3640999a7f96bbcc3ee23e39e851ba4b41"),
    ("massstorage.usbfd",         1904721920,    50592, "644a2a9966cc4137fe2f32b4ac430c7b13c0017d6cf37554a9763e65c022c862"),
    ("mounter.library",           1904775168,    67232, "52a08b60f694ad222fc1486bfba5b123912a509a13b1dcf35610fbb50cd0321a"),
    ("newlib.library.kmod",       1904844800,   797316, "53613e898c95b7f2a550d8fdaceb566f4676004d685f1ad01b43118fff2ebd5a"),
    ("nonvolatile.library.kmod",  1905852416,    11668, "78f7a2353d3803d916340241504ed8413dd5a682a107ac3fa2562b2cf3b6274c"),
    ("ohci.usbhcd",               1905866752,    67004, "de2b5e9e79068c574284d721f95a410f94e7c387884e7fc43ff579a57e0b9633"),
    ("p1022sata.device.kmod",     1905958912,   149344, "214ece42d78fda7a287ee57f12e43b22d0eb14d8252c4e6d5fb4adc335e1b4db"),
    ("petunia.library.kmod",      1906167808,  1164004, "822cec653f746ce5129484e25533cedd1f0e37a090d0ef595d2fe5d06f9d551b"),
    ("ram-handler.kmod",          2145304576,    44148, "1ab7a694e39738767dd7fe93a352739d4664934f2de34c61c6fe48ed90c005ba"),
    ("ramdrive.device.kmod",      2145351680,    14992, "f3e87eb0f9084c07911ff18ebd995f7811090dc467914206c3e6dd7b7bb4fed4"),
    ("ramlib.kmod",               2145370112,    15752, "3870630812c75ada6af7654acac98c784359c1ebde872f47ab4ddb449936e44b"),
    ("shell.kmod",                2145388544,   133528, "b12a27a92e1862763e1474b799baae2c2697bcc286554edb53c12993ac50ccb8"),
    ("strap.kmod",                2146031616,    16564, "54f1be5b5e131ce9438158c1f3e066b6f93980d6e8d3b43f2dceecab9d719b61"),
    ("timer.device.kmod",         2146052096,    16920, "01832f7cb6f3a9570aff636329c646cbe1b5f9b76b10512324479ffc8ac13d78"),
    ("uhci.usbhcd",               2146072576,    79360, "172c5ecf86e09c08138a992eaa4ac5f4fcdb2dab3afe2f7274ffddb6393a3b28"),
    ("usbresource.library",       2146154496,    11328, "7073b94cd1f9a034e9923c6dca34f8a93314ab5cd8635ba1f38471f14e90a51c"),
    ("usbsys.device",             2146168832,   139580, "a89b41157931d3fc40a4c8e5f1a52e8efdf28371069e528a5e91f0dd84011480"),
    ("Kicklayout",                  1904605184,     3644, "a3f80680d071070d63e5f6ba329e18c02dad1a5b1ea659bdf3e0dc7b85e91419"),
]

RADEONRX = 'MODULE Kickstart/RadeonRX.chip'


def main():
    p = argparse.ArgumentParser(
        description=__doc__.split('\n\n')[0],
        epilog=__doc__.split('\n\n', 2)[2],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('-o', '--output', default='Kickstart-a1222.zip',
                   help='zip archive to make (default: Kickstart-a1222.zip)')
    p.add_argument('image', help='the A1222 recovery image, ' + IMAGE_NAME)
    args = p.parse_args()

    if os.path.getsize(args.image) != IMAGE_SIZE:
        sys.exit('%s is not %s (MD5 %s): it has the wrong size'
                 % (args.image, IMAGE_NAME, IMAGE_MD5))
    with tempfile.TemporaryDirectory() as tmp, open(args.image, 'rb') as img:
        for name, offset, size, sha256 in FILES:
            img.seek(offset)
            data = img.read(size)
            if hashlib.sha256(data).hexdigest() != sha256:
                sys.exit('%s does not match %s in the image: is it %s '
                         '(MD5 %s)?' % (name, sha256[:16], IMAGE_NAME,
                                        IMAGE_MD5))
            if name == 'Kicklayout':
                text = data.decode('latin-1')
                assert text.count(RADEONRX + '\n') == 1
                data = text.replace(
                    RADEONRX + '\n',
                    ';' + RADEONRX +
                    ' - fragmented in the recovery image\n').encode('latin-1')
            with open(os.path.join(tmp, name), 'wb') as f:
                f.write(data)
        mkkickzip.make(argparse.Namespace(source=tmp, module=[],
                                          output=args.output))


if __name__ == '__main__':
    main()
