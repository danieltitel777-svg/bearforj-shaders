from PIL import Image, ImageOps, ImageDraw
import math, os
src=Image.open('/workspace/brand/mishmish_etalon_helmet.jpg').convert('L')
def figure(box):
    fig=src.crop(box)
    alpha=ImageOps.invert(fig).point(lambda v:255 if v>70 else 0)
    outer=fig.point(lambda v:255 if v>200 else 0); ImageDraw.floodfill(outer,(0,0),128)
    inside=outer.point(lambda v:255 if v==255 else 0)
    rgba=Image.new('RGBA',fig.size,(0,0,0,0))
    rgba.paste(Image.new('RGBA',fig.size,(0,0,0,255)),(0,0),inside)
    rgba.paste(Image.new('RGBA',fig.size,(235,235,235,255)),(0,0),alpha)
    return rgba.crop(rgba.getbbox())
side=ImageOps.mirror(figure((675,100,925,590)))
front=figure((30,100,345,590))
out='src/package/Interface/CommunityShaders/Bearforj/anim'; os.makedirs(out,exist_ok=True)
W,H,N=160,200,10
def sheet(base,fn,mode):
    s=Image.new('RGBA',(W*N,H),(0,0,0,0))
    b=base.resize((int(base.width*170/base.height),170),Image.LANCZOS)
    for i in range(N):
        t=i/N*2*math.pi
        f=b
        if mode=='push': f=f.rotate(-14-8*math.sin(t),resample=Image.BICUBIC,expand=True)
        else: f=f.rotate(4*math.sin(t),resample=Image.BICUBIC,expand=True)
        sq=1+0.12*math.sin(2*t)
        f=f.resize((int(f.width*sq),int(f.height/sq)),Image.LANCZOS)
        f.thumbnail((W,H))
        bob=int(4*abs(math.sin(t)))
        s.alpha_composite(f,(i*W+(W-f.width)//2,H-f.height-bob))
    s.save(f'{out}/{fn}')
    p=Image.new('RGBA',s.size,(0,0,0,255)); p.alpha_composite(s); p.convert('RGB').resize((800,100)).save(f'/workspace/bearforj/prev_{fn}')
sheet(side,'push_sheet.png','push')
sheet(front,'walk_sheet.png','walk')
