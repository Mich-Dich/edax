# Renders printable ASCII of a monospace font into an 8-bit coverage atlas for the UI spike.
from PIL import Image, ImageDraw, ImageFont
import sys
font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 14)
cw, ch = 9, 18
cols, rows = 16, 6
img = Image.new("L", (cols * cw, rows * ch), 0)
d = ImageDraw.Draw(img)
asc, desc = font.getmetrics()
top = (ch - (asc + desc)) // 2
for k in range(95):
    c = chr(32 + k)
    x, y = (k % cols) * cw, (k // cols) * ch
    d.text((x, y + top), c, font=font, fill=255)
# slot 95: solid white, sampled for untextured shapes
x, y = (95 % cols) * cw, (95 // cols) * ch
d.rectangle([x, y, x + cw - 1, y + ch - 1], fill=255)
open(sys.argv[1], "wb").write(img.tobytes())
img.save("/tmp/spike_font.png")
print(img.size, asc, desc)
