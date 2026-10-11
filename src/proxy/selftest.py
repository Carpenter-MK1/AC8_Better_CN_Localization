# -*- coding: utf-8 -*-
"""从 proxy/ac8proxy.c 里「抽出真实的」包名匹配逻辑，生成一个自测程序。
   —— 手抄容易漂移，抽取能保证测的就是编进 DLL 的那份代码。"""
import io
import re

import os

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "ac8proxy.c")
OUT = os.path.join(HERE, "_test_match.c")

src = io.open(SRC, encoding="utf-8").read()


def grab(pat, name):
    m = re.search(pat, src, re.S)
    assert m, "抽不到：" + name
    return m.group(0)


parts = [
    grab(r"static const wchar_t kPakPrefix\[\][^;]*;", "kPakPrefix"),
    grab(r"static const wchar_t kPakSuffix\[\][^;]*;", "kPakSuffix"),
    grab(r"#define PAK_SUFFIX_LEN \d+", "PAK_SUFFIX_LEN"),
    grab(r"static wchar_t lower_w\(wchar_t c\)\s*\{.*?\n\}", "lower_w"),
    grab(r"static int is_mod_pak\(const wchar_t\* path\)\s*\{.*?\n\}", "is_mod_pak"),
]

BODY = r"""
#include <stdio.h>
#include <wchar.h>

__PARTS__

struct C { const wchar_t* p; int e; const char* tag; };

int main(void)
{
    static const struct C cs[] = {
        { L"../../../Game/Content/Paks/pakchunk0-Windows_998_P.pak", 1, "our-998"          },
        { L"pakchunk0-Windows_999_P.pak",                            1, "thai-999(bare)"   },
        { L"C:\\X\\Paks\\pakchunk0-Windows_999_P.pak",               1, "thai-999(winpath)"},
        { L"pakchunk0-Windows_1_P.pak",                              1, "1-char-mid"       },
        { L"pakchunk0-Windows__P.pak",                               0, "empty-mid(no)"    },
        { L"pakchunk0-Windows_998_p.PAK",                            1, "mixed-case"       },
        { L"PAKCHUNK0-WINDOWS_998_P.PAK",                            1, "all-upper"        },

        { L"pakchunk0-Windows.pak",                                  0, "official-main"    },
        { L"pakchunk0optional-Windows.pak",                          0, "official-optional"},
        { L"pakchunk0-Windows.sig",                                  0, "sig-file"         },
        { L"pakchunk0-Windows_998_P.pak.bak",                        0, "extra-tail"       },
        { L"pakchunk10-Windows_999_P.pak",                           0, "other-chunk"      },
        { L"pakchunk0-Windows__P.pakX",                              0, "wrong-suffix"     },
        { L"global.utoc",                                            0, "utoc"             },
        { L"",                                                       0, "empty"            },
    };
    int n = (int)(sizeof(cs) / sizeof(cs[0])), bad = 0;
    for (int i = 0; i < n; ++i) {
        int got = is_mod_pak(cs[i].p);
        int ok = (got == cs[i].e);
        printf("  %-18s got=%d expect=%d   %s\n", cs[i].tag, got, cs[i].e,
               ok ? "OK" : "** FAIL **");
        if (!ok) ++bad;
    }
    printf("\n  %s  (%d/%d)\n", bad ? "有失败" : "全部通过", n - bad, n);
    return bad ? 1 : 0;
}
"""

io.open(OUT, "w", encoding="utf-8", newline="\n").write(
    BODY.replace("__PARTS__", "\n".join(parts)))
print("已生成：" + OUT)
for p in parts:
    print("  抽出:", p.splitlines()[0][:70])
