"""Contact sheet of shots/smoke/*.png -> shots/smoke_sheet.png"""
import glob, os
from PIL import Image, ImageDraw
fs = sorted(glob.glob('shots/smoke/*.png'))
cols, tw, th = 5, 240, 160
rows = (len(fs) + cols - 1) // cols
sheet = Image.new('RGB', (cols * tw, rows * (th + 14)), (40, 40, 40))
d = ImageDraw.Draw(sheet)
for i, f in enumerate(fs):
    im = Image.open(f).convert('RGB').resize((tw, th))
    x, y = (i % cols) * tw, (i // cols) * (th + 14)
    sheet.paste(im, (x, y + 14))
    d.text((x + 3, y + 1), os.path.basename(f)[:-4], fill=(255, 255, 0))
sheet.save('shots/smoke_sheet.png')
print(len(fs), 'images')
