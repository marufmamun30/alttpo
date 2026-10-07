# Contact sheet of test screenshots: sheet.py <out.png> <cols> <scale%> <file>...
import sys
from PIL import Image, ImageDraw
out, cols, scale = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
files = sys.argv[4:]
ims = []
for f in files:
    im = Image.open(f).convert("RGB")
    im = im.resize((im.width * scale // 100, im.height * scale // 100), Image.NEAREST)
    d = ImageDraw.Draw(im)
    label = f.replace("\\", "/").split("/")[-1].replace(".png", "")
    d.rectangle([0, 0, 6 * len(label) + 4, 10], fill=(0, 0, 0))
    d.text((2, 0), label, fill=(255, 255, 0))
    ims.append(im)
w, h = ims[0].size
rows = (len(ims) + cols - 1) // cols
sheet = Image.new("RGB", (cols * w, rows * h))
for i, im in enumerate(ims):
    sheet.paste(im, ((i % cols) * w, (i // cols) * h))
sheet.save(out)
