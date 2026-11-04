"""Contact sheet of host pictures: python3 sheet.py out.png cols width frame1.ppm frame2.ppm ..."""
import sys
from PIL import Image
out, cols, w = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
ims = [Image.open(f).convert('RGB') for f in sys.argv[4:]]
h = ims[0].size[1] * w // ims[0].size[0]
c = Image.new('RGB', (cols * w, ((len(ims) + cols - 1) // cols) * h))
for i, im in enumerate(ims):
    c.paste(im.resize((w, h)), ((i % cols) * w, (i // cols) * h))
c.save(out)
