from PIL import Image, ImageDraw, ImageFont, ImageOps
import json, os, sys
OUT=sys.argv[1]; os.makedirs(OUT,exist_ok=True)
F='/workspace/brand/fonts/Cinzel.ttf'
mono=[p for p in ['/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf'] if os.path.exists(p)]
def font(sz,m=False): return ImageFont.truetype(mono[0] if (m and mono) else F if not m else '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',sz)
heads={'happy':'/workspace/bearforj_ui/heads/head_happy.png','grumpy':'/workspace/bearforj_ui/heads/head_grumpy.png',
 'thinking':'/workspace/brand/mishmish_emotions/thinking.png','proud':'/workspace/brand/mishmish_emotions/proud.png',
 'wink':'/workspace/brand/mishmish_emotions/wink.png','surprised':'/workspace/brand/mishmish_emotions/surprised.png'}
etalon=Image.open('/workspace/brand/mishmish_etalon_helmet.jpg').convert('L').crop((30,100,345,590))
def card(fn,title,sub,tag,emo):
    W,H=800,300
    im=Image.new('L',(W,H),0); d=ImageDraw.Draw(im)
    d.rectangle((6,6,W-7,H-7),outline=189,width=2)
    if emo=='full':
        a=ImageOps.invert(etalon); a=a.resize((int(a.width*260/a.height),260)); im.paste(a,(W-a.width-30,20))
    else:
        h=Image.open(heads[emo]).convert('LA'); bg=Image.new('L',h.size,255); bg.paste(h.convert('L'),(0,0),h.split()[1])
        h=ImageOps.invert(bg).resize((230,230)); im.paste(h,(W-260,35))
    d.text((40,40),tag,fill=138,font=font(18,True))
    d.text((40,80),title,fill=255,font=font(46))
    y=160
    for line in sub.split('\n'):
        d.text((40,y),line,fill=200,font=font(22,True)); y+=32
    im.convert('RGB').save(f'{OUT}/{fn}.png')
idx=json.load(open('/workspace/bearforj/tools_bf/profiles_index.json'))
card('header','BearForj Shaders','v0.6  //  Open Shaders 2.19 fork\nВыбери погоду и свет — Миш Миш подстроится','SYS.01 // INSTALLER','full')
card('core','Core','DLL, шейдеры, меню, текстуры\nОбязательно','PKG.00 // CORE','proud')
card('weather','Погода / Weather','Один погодный мод. Не смешивай.\nPick ONE weather mod','PKG.01 // WEATHER','thinking')
card('lighting','Свет / Lighting','Можно несколько (Lux + Orbis + Via)\nSeveral allowed','PKG.02 // LIGHTING','wink')
card('options','Опции / Options','Меню для продвинутых\nAdvanced menu','PKG.03 // OPTIONS','surprised')
card('auto','Автоопределение','Ничего не ставить: Bearforj найдёт\nмоды по плагинам сам','PKG.01 // AUTO','happy')
for pid,name,kind,url,plugins in idx:
    emo={'weather':'happy','storm':'surprised','post':'thinking'}.get(kind,'wink')
    card(pid,name[:26],({'weather':'Weather profile','storm':'Storm profile','post':'Post-processing profile'}.get(kind,'Lighting profile'))+'\n'+(('Nexus #'+url.rsplit('/',1)[-1]) if url else 'Bearforj default'),f'PRF // {kind.upper()}',emo)
card('advanced','Advanced menu','Отладка и все ползунки видны сразу\nDebug tools visible','OPT // ADVANCED','grumpy')
print('ok')
