import sys
src, dst = sys.argv[1], sys.argv[2]
n_form2 = 0
with open(src, 'rb') as f, open(dst, 'wb') as o:
    i = 0
    while True:
        s = f.read(2352)
        if len(s) < 2352:
            break
        # MODE2 XA: subheader at 16..23; submode byte 18 bit 5 = form 2
        if s[18] & 0x20:
            n_form2 += 1
        o.write(s[24:24+2048])
        i += 1
print('sectors', i, 'form2 sectors', n_form2)
