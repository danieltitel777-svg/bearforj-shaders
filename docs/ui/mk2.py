# Bearforj Shaders 0.6 UI mockup v2 (strict B/W, VoiceOver style). Usage: python3 mk2.py
from PIL import Image, ImageDraw, ImageFont
FD='/workspace/bearforj/src/package/Interface/CommunityShaders/Fonts/'
B='/workspace/brand/BearForj_brand/'
def mono(s): return ImageFont.truetype(FD+'IBMPlexMono/IBMPlexMono-Regular.ttf',s)
def jost(s): return ImageFont.truetype(FD+'Jost/Jost-Regular.ttf',s)
def img(p,s):
    i=Image.open(p).convert('RGBA'); i=i.crop(i.split()[3].getbbox()); i.thumbnail((s,s),Image.LANCZOS); return i
LOGO=B+'full/bearforj_logo_4k_transparent.png'
HAP='/workspace/brand/placeholders/head_happy.png'; GRU='/workspace/brand/placeholders/head_grumpy.png'
W,H=1280,780; T1,T2,BR='#e6e6e6','#8a8a8a','#3a3a3a'
def sp(d,xy,t,f,fill,ls=2):
    x,y=xy
    for ch in t: d.text((x,y),ch,font=f,fill=fill); x+=d.textlength(ch,font=f)+ls
    return x
def spw(d,t,f,ls=2): return sum(d.textlength(c,font=f)+ls for c in t)
def pill(d,b,t,on=False):
    d.rounded_rectangle(b,(b[3]-b[1])//2,fill='#161616' if on else '#000',outline='#fff' if on else '#bdbdbd')
    f=jost(14);w=d.textlength(t,font=f);d.text(((b[0]+b[2]-w)/2,b[1]+(b[3]-b[1]-18)/2),t,font=f,fill=T1)
def ruler(d,x,y,w):
    d.line((x,y,x+w,y),fill=BR)
    for i in range(0,w+1,10): d.line((x+i,y,x+i,y-(6 if i%50==0 else 3)),fill=BR)
def render(L,out):
    im=Image.new('RGBA',(W,H),'#000')
    bg=img(LOGO,760).rotate(-12,expand=True,resample=Image.BICUBIC)
    bg.putalpha(bg.split()[3].point(lambda v:int(v*0.07))); im.alpha_composite(bg,(W-bg.width+150,140))
    d=ImageDraw.Draw(im); d.rectangle((8,8,W-9,H-9),outline=BR)
    lg=img(LOGO,62); im.alpha_composite(lg,(24,16))
    sp(d,(100,18),'SYS.01 // SHADERS-ONLINE',mono(10),T2)
    d.text((100,32),'BearForj Shaders',font=jost(30),fill='#fff')
    x=W-30
    for t in ['EN','RU'][::-1]:
        on=t==L['code'].upper(); w=40; b=(x-w,30,x,52)
        d.rectangle(b,fill='#e6e6e6' if on else '#000',outline='#bdbdbd'); d.text((b[0]+12,b[1]+3),t,font=jost(13),fill='#000' if on else T1); x-=w+6
    sp(d,(x-34,36),'LANG',mono(10),T2); x-=46
    for t in ['OPEN 2.19','v0.6']:
        w=spw(d,t,mono(10),1)+22; d.rectangle((x-w-10,26,x-10,56),outline='#bdbdbd'); sp(d,(x-w+1,36),t,mono(10),T1,1); x-=w+8
    ruler(d,24,84,W-48)
    x,y=24,96
    for i,t in enumerate(L['tabs']):
        f=mono(11); w=spw(d,t,f)+26; on=i==0
        d.rectangle((x,y,x+w,y+30),fill='#000',outline='#e6e6e6' if on else BR); sp(d,(x+13,y+8),t,f,'#fff' if on else '#9a9a9a'); x+=w+4
    d.rectangle((24,126,W-25,H-64),outline=BR)
    # left list
    d.rectangle((40,142,370,H-80),outline=BR)
    d.text((52,150),L['features'],font=jost(18),fill='#fff'); sp(d,(250,156),'FTR.%02d // LIST'%len(L['feats']),mono(10),T2)
    for i,(n,en) in enumerate(L['feats']):
        yy=184+i*44; sel=i==0
        if sel: d.rectangle((41,yy-4,369,yy+38),fill='#262626')
        h=img(HAP if en else GRU,34)
        if not en: h.putalpha(h.split()[3].point(lambda v:int(v*0.55)))
        im.alpha_composite(h,(52,yy))
        d.text((96,yy+6),n,font=jost(15),fill='#fff' if en else '#7a7a7a')
        # toggle
        tx=318; d.rounded_rectangle((tx,yy+9,tx+38,yy+29),10,outline='#bdbdbd',fill='#e6e6e6' if en else '#000')
        cx=tx+28 if en else tx+10; d.ellipse((cx-7,yy+12,cx+7,yy+26),fill='#000' if en else '#8a8a8a')
    # right settings
    X=390; sel=L['feats'][0][0]
    d.text((X,142),sel,font=jost(22),fill='#fff'); sp(d,(W-230,150),'FEAT.01 // PARAMS',mono(10),T2)
    ruler(d,X,180,W-X-45); yy=196
    for kind,*r in L['settings']:
        if kind=='h': sp(d,(X,yy+4),r[0],mono(10),T2); d.line((X,yy+22,W-45,yy+22),fill='#1e1e1e'); yy+=34; continue
        d.text((X,yy),r[0],font=jost(14),fill=T1)
        if kind=='s':
            v,lo,hi=r[1:4]; x0,x1=X+300,W-150; fr=(v-lo)/(hi-lo)
            d.line((x0,yy+10,x1,yy+10),fill=BR,width=2); d.line((x0,yy+10,x0+(x1-x0)*fr,yy+10),fill='#e6e6e6',width=2)
            xx=x0+(x1-x0)*fr; d.rectangle((xx-4,yy+3,xx+4,yy+17),fill='#fff'); sp(d,(x1+16,yy+2),f'{v:.2f}',mono(11),T1,1)
        elif kind=='c':
            b=(X+300,yy-2,X+316,yy+14); d.rectangle(b,outline='#bdbdbd',fill='#e6e6e6' if r[1] else '#000')
        yy+=32
    # tooltip with emote
    tx,ty=X+330,yy+6; d.rectangle((tx,ty,tx+420,ty+76),fill='#0d0d0d',outline='#bdbdbd')
    e=img(B+'MishMish/emotions/thinking.png',64); im.alpha_composite(e,(tx+8,ty+6))
    sp(d,(tx+82,ty+10),'TIP // HINT',mono(9),T2); d.multiline_text((tx+82,ty+26),L['tip'],font=jost(13),fill=T1,spacing=3)
    # empty-state panel (bottom-right card)
    ey=H-200; d.rectangle((X,ey,X+330,H-96),outline=BR)
    e=img(B+'MishMish/emotions/surprised.png',70); im.alpha_composite(e,(X+10,ey+12))
    sp(d,(X+92,ey+16),'EMPTY // SEARCH',mono(9),T2); d.multiline_text((X+92,ey+34),L['empty'],font=jost(13),fill=T1,spacing=3)
    # footer
    fy=H-56
    for i,t in enumerate(L['btns']):
        bx=40+i*190; pill(d,(bx,fy+8,bx+178,fy+38),t)
    e=img(B+'MishMish/emotes/generation.png',40); im.alpha_composite(e,(W-470,fy+2))
    d.rectangle((W-420,fy+12,W-130,fy+34),outline='#bdbdbd'); d.rectangle((W-418,fy+14,W-418+180,fy+32),fill='#3a3a3a')
    sp(d,(W-360,fy+16),L['loading'],mono(10),T1,1); sp(d,(W-112,fy+18),'LAT 64 N',mono(10),T2,1)
    im.convert('RGB').save(out); print(out)
TABS_EN=['CHARACTER','SKY & WEATHER','LIGHTING','LANDSCAPE','PARTICLES','WATER','POST-PROCESSING','SYSTEM']
TABS_RU=['ПЕРСОНАЖ','НЕБО И ПОГОДА','ОСВЕЩЕНИЕ','ЛАНДШАФТ','ЧАСТИЦЫ','ВОДА','ПОСТОБРАБОТКА','СИСТЕМА']
EN=dict(code='en',tabs=TABS_EN,features='Features',
 feats=[('Submersion Wetness',1),('Hair Backlighting',1),('Fur (Open)',1),('Skin',1),('Hair Specular',0),('Subsurface Scattering',1)],
 settings=[('h','WETNESS // RAIN'),('s','Rain wetness on characters',0.85,0,1),('s','Hair wetness boost',1.40,0,2),('s','Armor wetness boost',1.20,0,2),('c','Shelter check (no rain under roofs)',1),('h','SURFACE RAIN FLOW'),('c','Drops on roofs and walls',1),('s','Flow speed',0.60,0,1),('s','Drip density',0.45,0,1)],
 tip='Shelter check: characters under roofs\nand awnings stay dry. Restart not needed.',
 empty='No features match "fog".\nTry another word, Mish Mish is looking.',
 btns=['Save','Reset to defaults','Clear shader cache'],loading='COMPILING 1240/2210')
RU=dict(code='ru',tabs=TABS_RU,features='Фичи',
 feats=[('Намокание персонажа',1),('Подсветка волос',1),('Мех (Open)',1),('Кожа',1),('Блеск волос',0),('Подповерхностное рассеяние',1)],
 settings=[('h','НАМОКАНИЕ // ДОЖДЬ'),('s','Намокание персонажей под дождём',0.85,0,1),('s','Усиление намокания волос',1.40,0,2),('s','Усиление намокания брони',1.20,0,2),('c','Проверка укрытия (под крышей сухо)',1),('h','СТЕКАНИЕ ДОЖДЯ'),('c','Капли на крышах и стенах',1),('s','Скорость стекания',0.60,0,1),('s','Плотность капель',0.45,0,1)],
 tip='Проверка укрытия: под крышами и навесами\nперсонажи остаются сухими. Перезапуск не нужен.',
 empty='Ничего не найдено по «туман».\nПопробуй другое слово, Миш Миш ищет.',
 btns=['Сохранить','Сброс на дефолт','Сброс кэша шейдеров'],loading='КОМПИЛЯЦИЯ 1240/2210')
render(RU,'/workspace/bearforj_ui/mockup_ru_v2.png'); render(EN,'/workspace/bearforj_ui/mockup_en_v2.png')
