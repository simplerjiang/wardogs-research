import pefile
p = r"C:\Windows\System32\ci.dll"
pe = pefile.PE(p, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
for e in pe.DIRECTORY_ENTRY_IMPORT:
    print("==", e.dll.decode())
    for imp in e.imports:
        n = imp.name.decode() if imp.name else "ord%d" % imp.ordinal
        print("   %-46s iat_rva=0x%X" % (n, imp.address - pe.OPTIONAL_HEADER.ImageBase))
