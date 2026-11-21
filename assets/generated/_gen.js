const fs = require('fs');
const dir = require('path').resolve(__dirname);

const commonDefs = `
<linearGradient id="sky" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#0A1B33"/><stop offset="1" stop-color="#14406B"/></linearGradient>
<linearGradient id="card" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#161A22"/><stop offset="1" stop-color="#1E2431"/></linearGradient>
<linearGradient id="purple" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#7C4DFF"/><stop offset="1" stop-color="#B06AB3"/></linearGradient>
<linearGradient id="cloud" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#FFFFFF"/><stop offset="1" stop-color="#C9C3F5"/></linearGradient>
<radialGradient id="glow"><stop offset="0" stop-color="#7C4DFF" stop-opacity="0.35"/><stop offset="1" stop-color="#7C4DFF" stop-opacity="0"/></radialGradient>
`;

const F = "'Microsoft YaHei','Segoe UI',sans-serif";
const t = (x, y, s, size, fill, anchor, weight) => `<text x="${x}" y="${y}" font-family="${F}" font-size="${size}" fill="${fill}" text-anchor="${anchor || 'start'}" font-weight="${weight || 'normal'}">${s}</text>`;
const rr = (x, y, w, h, r, fill, stroke, sw) => `<rect x="${x}" y="${y}" width="${w}" height="${h}" rx="${r}" fill="${fill}" ${stroke ? `stroke="${stroke}" stroke-width="${sw || 1}"` : ''}/>`;
const bg = (c) => `<rect width="480" height="480" fill="${c || '#0B0E14'}"/>`;
const status = (right) => `${t(16, 22, '10:08', 14, '#E8EDF7', 'start', 'bold')}${t(240, 22, right || 'ESP32-P4C5', 13, '#7E8BA5', 'middle')}${t(464, 22, 'WiFi', 13, '#4DD0E1', 'end')}`;

const panels = [
  {
    name: '01-desktop',
    body: `
${bg()}
${status('桌面 1/2')}
${t(240, 60, '手机式桌面 · 一页 6 个 App', 13, '#55637D', 'middle')}
${[0,1,2].map(i => `<circle cx="${210 + i*30}" cy="78" r="3.5" fill="${i===0?'#7C4DFF':'#334058'}"/>`).join('')}
${[
  {x:88,y:160,c:'#4DD0E1',label:'监控',g:'<circle cx="0" cy="-8" r="10" fill="#0B0E14" stroke="#4DD0E1" stroke-width="2"/><circle cx="0" cy="-8" r="4" fill="#4DD0E1"/>'},
  {x:240,y:160,c:'#FF6B9D',label:'相机',g:'<circle cx="0" cy="-8" r="12" fill="#2A1730" stroke="#FF6B9D" stroke-width="2"/><circle cx="0" cy="-8" r="6" fill="#FF6B9D"/>'},
  {x:392,y:160,c:'#FFC857',label:'相册',g:rr(-12,-20,24,20,3,'#3A2E14','#FFC857',2)+'<circle cx="-5" cy="-14" r="2" fill="#FFC857"/>'},
  {x:88,y:310,c:'#FF5C6C',label:'交易',g:'<path d="M-14 -2 L-6 -12 L2 -6 L14 -18" stroke="#FF5C6C" stroke-width="3" fill="none"/>'},
  {x:240,y:310,c:'#4DA3FF',label:'天气',g:'<circle cx="-5" cy="-14" r="7" fill="#FFC857"/><ellipse cx="5" cy="-12" rx="12" ry="7" fill="#C9C3F5"/>'},
  {x:392,y:310,c:'#8E9BB5',label:'设置',g:'<circle cx="0" cy="-10" r="9" fill="none" stroke="#8E9BB5" stroke-width="3"/><circle cx="0" cy="-10" r="3" fill="#8E9BB5"/>'}
].map(a => rr(a.x-40,a.y-50,80,80,18,'#151B27','#2A3345') + `<g transform="translate(${a.x},${a.y-10})">${a.g}</g>` + t(a.x, a.y+48, a.label, 15, '#DCE5F5', 'middle', 'bold')).join('')}
${rr(60,428,360,32,16,'#111722','#2A3345')}
${t(240,449,'^  上滑锁屏待机 / 左右滑动翻页',13,'#7E8BA5','middle')}
`
  },
  {
    name: '02-lockscreen',
    body: `
${bg('url(#sky)')}
${[...Array(30)].map((_,i)=>`<circle cx="${(i*83)%470+5}" cy="${(i*137)%400+30}" r="${i%4===0?1.6:1}" fill="#BBD4FF" opacity="${0.25+(i%5)*0.1}"/>`).join('')}
${t(240,62,'星期四  11月19日',16,'#A9C2E8','middle')}
${t(240,215,'10:08',96,'#FFFFFF','middle','bold')}
${rr(120,250,240,74,20,'url(#card)','#2A3345')}
${t(140,282,'深圳  小雨  25°C',19,'#E8EDF7','start','bold')}
${t(140,307,'湿度 68%   东南风   体感 24°',13,'#8E9BB5')}
${[...Array(40)].map((_,i)=>`<circle cx="${30+((i*97)%420)}" cy="${330+((i*53)%130)}" r="${2+(i%3)}" fill="${['#7C4DFF','#4DD0E1','#FF6B9D'][i%3]}" opacity="0.55"/>`).join('')}
${t(240,455,'上滑解锁',14,'#A9C2E8','middle')}
${t(240,472,'⌃',18,'#7C93B8','middle')}
`
  },
  {
    name: '03-control-center',
    body: `
${bg('#0B0E14')}
${rr(0,0,480,480,0,'#0B0E14')}
${t(20,40,'控制中心',22,'#FFFFFF','start','bold')}
${t(20,70,'点击面板任意位置或底部按钮收起',12,'#7E8BA5')}
${t(20,108,'亮度',14,'#DCE5F5')}${rr(80,98,340,10,5,'#26304A')}${rr(80,98,272,10,5,'#7C4DFF')}${t(450,108,'80',13,'#7C4DFF','end')}
${rr(16,132,448,122,16,'url(#card)','#2A3345')}
${t(34,160,'WiFi',16,'#E8EDF7','start','bold')}
${rr(300,144,124,34,12,'#1B2334','#3D4B6D')}${t(362,166,'扫描 WiFi',13,'#9D7CFF','middle')}
${t(34,190,'MyWiFi   已连接   192.168.1.x',13,'#4DD0E1')}
${t(34,218,'TP-LINK_5G · 点此查看附近网络',12,'#8E9BB5')}
${rr(16,268,448,60,16,'url(#card)','#2A3345')}
${t(34,304,'蓝牙  ESP32-P4C5',15,'#E8EDF7','start','bold')}${rr(392,284,50,24,12,'#4DD0E1')}<circle cx="429" cy="296" r="9" fill="#0B0E14"/>
${rr(16,342,448,60,16,'url(#card)','#2A3345')}
${t(34,378,'WiFi 射频',15,'#E8EDF7','start','bold')}${t(300,378,'关闭可省电',12,'#7E8BA5')}${rr(392,358,50,24,12,'#3A4560')}<circle cx="405" cy="370" r="9" fill="#C7D1E5"/>
${rr(60,430,360,34,17,'url(#purple)')}${t(240,452,'点击收起',15,'#FFFFFF','middle','bold')}
`
  },
  {
    name: '04-monitor',
    body: `
${bg('#101820')}
<path d="M0 480 L160 180 L320 180 L480 480 Z" fill="#1A2530"/>
<path d="M0 480 L120 210 M480 480 L360 210" stroke="#2B3A4A" stroke-width="3" fill="none"/>
${rr(170,170,140,180,6,'#233240','#3B5064',2)}
${rr(196,196,88,90,4,'#152330','#46607A')}
<circle cx="240" cy="336" r="24" fill="#31465A"/><path d="M204 420 Q240 350 276 420 Z" fill="#31465A"/>
${[...Array(12)].map((_,i)=>`<rect x="0" y="${i*40}" width="480" height="1" fill="#5B7186" opacity="0.12"/>`).join('')}
${rr(12,12,456,34,10,'#0B0E14CC')}${t(28,35,'● 监控中   RTSP 1 client',14,'#FF5C6C','start','bold')}${t(460,35,'192.168.1.x:554',12,'#8E9BB5','end')}
${rr(12,434,456,34,10,'#0B0E14CC')}${t(28,456,'SC2336  1280x720  30fps   H.264 硬编',12,'#4DD0E1')}${t(460,456,'延迟 120ms',12,'#8E9BB5','end')}
`
  },
  {
    name: '05-camera',
    body: `
${bg('#101820')}
<path d="M0 480 L140 150 L340 150 L480 480 Z" fill="#1A2530"/>
${rr(190,150,100,190,5,'#233240','#3B5064',2)}
<circle cx="240" cy="365" r="26" fill="#31465A"/><path d="M198 455 Q240 380 282 455 Z" fill="#31465A"/>
${rr(136,96,208,208,4,'none','#4DD0E1',1.5)}
${t(20,34,'相机',18,'#FFFFFF','start','bold')}${t(460,34,'1:1  ·  640x640',13,'#4DD0E1','end')}
${rr(12,412,116,46,12,'#1B2532','#2A3345')}${t(70,441,'1:1',15,'#7C4DFF','middle','bold')}
${rr(136,412,116,46,12,'#1B2532','#2A3345')}${t(194,441,'4:3',15,'#C7D1E5','middle')}
${rr(260,412,116,46,12,'#1B2532','#2A3345')}${t(318,441,'16:9',15,'#C7D1E5','middle')}
<circle cx="432" cy="435" r="25" fill="none" stroke="#FF6B9D" stroke-width="4"/><circle cx="432" cy="435" r="15" fill="#FF6B9D"/>
`
  },
  {
    name: '06-gallery',
    body: `
${bg()}
${t(20,36,'相册',20,'#FFFFFF','start','bold')}${t(460,36,'24 张',13,'#7E8BA5','end')}
${rr(300,16,72,28,14,'#7C4DFF')}${t(336,35,'列表',13,'#FFFFFF','middle')}
${rr(380,16,80,28,14,'#1B2532','#2A3345')}${t(420,35,'图标',13,'#C7D1E5','middle')}
${[[24,70],[252,70],[24,240],[252,240]].map((p,i)=>{
  const g=['#1E3A5F','#3A2A55','#173F3A','#4A2A2A'][i];
  return rr(p[0],p[1],204,150,10,g,'#2A3345')+`<circle cx="${p[0]+150}" cy="${p[1]+38}" r="14" fill="#FFC857" opacity="0.8"/><path d="M${p[0]+10} ${p[1]+140} L${p[0]+70} ${p[1]+70} L${p[0]+120} ${p[1]+120} L${p[0]+160} ${p[1]+85} L${p[0]+194} ${p[1]+140} Z" fill="#0B0E14" opacity="0.35"/>`;
}).join('')}
${rr(24,422,432,40,12,'#111722','#2A3345')}
${t(90,448,'‹  上一页',13,'#C7D1E5','middle')}${t(240,448,'1 / 6',14,'#7C4DFF','middle','bold')}${t(390,448,'下一页  >',13,'#C7D1E5','middle')}
`
  },
  {
    name: '07-stock',
    body: `
${bg()}
${t(16,30,'‹  返回',13,'#8E9BB5')}${t(180,30,'宁德时代  300750',17,'#FFFFFF','middle','bold')}${rr(356,16,48,22,11,'#7C4DFF')}${t(380,31,'A股',12,'#FFFFFF','middle','bold')}${rr(410,16,48,22,11,'#1B2532','#2A3345')}${t(434,31,'美股',12,'#C7D1E5','middle')}
${t(16,78,'218.65',36,'#FF5C6C','start','bold')}${t(464,78,'+2.34%',18,'#FF5C6C','end','bold')}
${[['今开','215.20'],['最高','221.80'],['成交量','12.6万手'],['昨收','213.65'],['最低','214.30'],['成交额','45.2亿']].map((v,i)=>{
  const x=12+(i%3)*153, y=94+Math.floor(i/3)*38;
  return rr(x,y,149,34,8,'#141B28','#2A3345')+t(x+10,y+22,v[0],12,'#7E8BA5')+t(x+139,y+22,v[1],13,'#DCE5F5','end');
}).join('')}
${rr(12,176,456,214,12,'#101722','#2A3345')}
${t(26,198,'日K  近180日  ·  东财源',12,'#8E9BB5')}
${['日','周','月','年'].map((v,i)=>rr(386+i*18,186,16,16,3,i===0?'#7C4DFF':'#1B2532')+t(394+i*18,198,v,10,i===0?'#FFFFFF':'#8E9BB5','middle')).join('')}
${[['7.50',218],['7.30',250],['7.10',282],['6.90',314]].map(v=>t(56,v[1],v[0],10,'#55637D','end')).join('')}
${[...Array(36)].map((_,i)=>`<line x1="${60+i*11}" y1="210" x2="${60+i*11}" y2="372" stroke="#1F2A3D" stroke-width="1"/>`).join('')}
${[...Array(36)].map((_,i)=>{const up=i%3!==0; const x=64+i*11; const y=300-Math.sin(i/3)*45-i*1.1; const c=up?'#FF5C6C':'#22C55E'; return `<line x1="${x}" y1="${y-12}" x2="${x}" y2="${y+16}" stroke="${c}" stroke-width="1"/>${rr(x-3,y,6,18,up?1:3,up?'none':c,up?c:'none',1.5)}`;}).join('')}
<path d="M64 320 Q160 280 240 292 T456 246" stroke="#FFC857" stroke-width="1.6" fill="none"/>
<path d="M64 342 Q160 310 240 318 T456 280" stroke="#FF6B9D" stroke-width="1.5" fill="none"/>
${rr(12,402,456,44,12,'url(#purple)')}${t(240,430,'价格提醒（可设多条）  已设 2 条',15,'#FFFFFF','middle','bold')}
`
  },
  {
    name: '08-weather',
    body: `
${bg('url(#sky)')}
${t(20,36,'深圳',20,'#FFFFFF','start','bold')}${t(460,36,'更新于 10:05',12,'#8BA6C8','end')}
${t(24,130,'25°',72,'#FFFFFF','start','bold')}
${t(28,164,'小雨  ·  空气质量 优',16,'#BBD4FF')}
<circle cx="365" cy="104" r="34" fill="#FFE9A8" opacity="0.95"/>
${[[330,100,24],[370,88,30],[400,108,22]].map(c=>`<circle cx="${c[0]}" cy="${c[1]}" r="${c[2]}" fill="url(#cloud)" opacity="0.92"/>`).join('')}
${[0,1,2,3].map(i=>`<line x1="${335+i*22}" y1="140" x2="${328+i*22}" y2="168" stroke="#4DA3FF" stroke-width="5" stroke-linecap="round"/>`).join('')}
${rr(16,196,448,116,20,'url(#card)','#2A3345')}
${[['湿度','68%'],['风向','东南风'],['体感','24°']].map((v,i)=>`${t(48+i*150,240,v[0],13,'#8E9BB5','middle')}${t(48+i*150,280,v[1],20,'#E8EDF7','middle','bold')}`).join('')}
${rr(16,330,448,126,20,'url(#card)','#2A3345')}
${t(36,360,'今天 / 明天 / 后天',14,'#E8EDF7','start','bold')}
${[['今天','小雨','25° / 22°'],['明天','多云','27° / 23°'],['后天','晴','26° / 21°']].map((v,i)=>`${t(90+i*130,398,v[0],13,'#8E9BB5','middle')}${t(90+i*130,424,v[1],14,'#BBD4FF','middle')}${t(90+i*130,448,v[2],13,'#FFFFFF','middle')}`).join('')}
`
  },
  {
    name: '09-map',
    body: `
${bg('#1B2430')}
${[...Array(7)].map((_,i)=>`<line x1="${i*80}" y1="0" x2="${i*80}" y2="480" stroke="#2A3542" stroke-width="1"/>`).join('')}
${[...Array(7)].map((_,i)=>`<line x1="0" y1="${i*80}" x2="480" y2="${i*80}" stroke="#2A3542" stroke-width="1"/>`).join('')}
${[['M0 300 L480 250','#33404E',14],['M180 0 L220 480','#33404E',12],['M340 0 L300 480','#33404E',10],['M0 90 L480 130','#2C3946',8]].map(r=>`<path d="${r[0]}" stroke="${r[1]}" stroke-width="${r[2]}" fill="none"/>`).join('')}
<path d="M240 360 L240 300 L300 300 L300 230 L360 230 L360 170" stroke="#4DA3FF" stroke-width="7" fill="none" stroke-linejoin="round"/>
<circle cx="240" cy="360" r="10" fill="#4DD0E1" stroke="#FFFFFF" stroke-width="3"/>
<path d="M360 170 a14 14 0 0 1 -14 -14 a14 14 0 0 1 28 0 a14 14 0 0 1 -14 14 Z M360 142 a8 8 0 1 0 0 16 a8 8 0 1 0 0 -16" fill="#FF5C6C"/>
${rr(12,12,456,38,12,'#0B0E14DD')}${t(28,37,'3.2 公里 · 约 8 分钟',15,'#FFFFFF','start','bold')}${t(460,37,'GCJ-02 高德瓦片',11,'#7E8BA5','end')}
${rr(408,120,48,40,10,'#0B0E14DD')}${t(432,146,'+',22,'#FFFFFF','middle')}
${rr(408,170,48,40,10,'#0B0E14DD')}${t(432,196,'-',22,'#FFFFFF','middle')}
${rr(16,430,100,36,12,'#7C4DFF')}${t(66,453,'定位 / 导航',13,'#FFFFFF','middle')}
${rr(364,430,100,36,12,'#111722','#2A3345')}${t(414,453,'校准',13,'#C7D1E5','middle')}
<g transform="rotate(-16 240 265)"><rect x="62" y="232" width="356" height="64" rx="10" fill="#0B0E14E6" stroke="#FF5C6C" stroke-width="3"/><text x="240" y="274" font-family="${F}" font-size="30" fill="#FF5C6C" text-anchor="middle" font-weight="bold">已冻结 · V2.38</text></g>
${t(240,414,'源码归档 docs/frozen/map-app/，桌面已注销「导航」',11,'#FF9B9B','middle')}
`
  },
  {
    name: '10-music',
    body: `
${bg('#0B0E14')}<circle cx="240" cy="180" r="230" fill="url(#glow)"/>
${t(20,34,'音乐',18,'#FFFFFF','start','bold')}${t(460,34,'网络曲库',12,'#7E8BA5','end')}
<circle cx="240" cy="152" r="80" fill="#1B2537" stroke="#3A4560" stroke-width="2"/>
<circle cx="240" cy="152" r="58" fill="#2A1D46"/>
<path d="M226 128 L226 168 Q226 180 214 180 Q206 180 206 173" stroke="#B79CFF" stroke-width="6" fill="none" stroke-linecap="round"/>
<circle cx="212" cy="173" r="8" fill="#B79CFF"/><circle cx="256" cy="164" r="8" fill="#B79CFF"/>
${t(240,262,'稻香',22,'#FFFFFF','middle','bold')}${t(240,286,'周杰伦  ·  免费曲目',13,'#8E9BB5','middle')}
${[['回家的路上', '#5D6B84'],['还记得你说家是唯一的城堡', '#8E9BB5'],['随着稻香河流继续奔跑', '#C7D1E5'],['微微笑  小时候的梦我知道', '#FFFFFF'],['不要哭让萤火虫带着你逃跑', '#8E9BB5']].map((v,i)=>t(240,320+i*21,v[0],12,v[1],'middle')).join('')}
${t(20,424,'1:24',10,'#7E8BA5')}${t(460,424,'3:32',10,'#7E8BA5','end')}
${rr(40,432,400,5,3,'#26304A')}${rr(40,432,168,5,3,'#7C4DFF')}
${t(100,462,'|‹',18,'#C7D1E5','middle')}<circle cx="240" cy="457" r="20" fill="url(#purple)"/>${t(240,464,'>',19,'#FFFFFF','middle')}${t(380,462,'>|',18,'#C7D1E5','middle')}
`
  },
  {
    name: '11-settings',
    body: `
${bg()}
${t(20,38,'设置',22,'#FFFFFF','start','bold')}${t(460,38,'系统 v2.32',12,'#7E8BA5','end')}
${[
  ['亮度','80',false],['音量','5',false],['分辨率','1280x720',false],['帧率','30 fps',false],
  ['屏幕方向','纵向',false],['蓝牙','开启',true],['TF 卡','7.3 GB 已挂载',false],['应用管理','8 个 · 后台/刷新率',false]
].map((v,i)=>{
  const y=58+i*44;
  return rr(12,y,456,40,10,'#141B28','#2A3345')+t(28,y+26,v[0],14,'#DCE5F5')+t(v[2]?386:452,y+26,v[1],13,v[2]?'#4DD0E1':'#8E9BB5','end')+(v[2]?rr(400,y+10,44,20,10,'#4DD0E1'):'');
}).join('')}
${rr(12,416,456,42,12,'#2A1B2E','#7C4DFF')}${t(240,443,'应用管理 → 交易 → 刷新率（前台 1/3/5 秒 · 后台 3/5/10 秒）',12,'#C7A8FF','middle')}
${rr(140,468,200,0,0,'transparent')}
`
  },
  {
    name: '12-selftest',
    body: `
${bg()}
${t(20,38,'开机自检',20,'#FFFFFF','start','bold')}${t(460,38,'BOOT CHECK',12,'#7E8BA5','end')}
<circle cx="240" cy="118" r="38" fill="none" stroke="#26304A" stroke-width="8"/>
<path d="M240 80 A38 38 0 0 1 277 118" stroke="#7C4DFF" stroke-width="8" fill="none" stroke-linecap="round"/>
${t(240,128,'检测中',14,'#C7D1E5','middle')}
${[['系统','正常'],['显示','正常'],['触摸','正常'],['内存','正常'],['网络','待连接'],['相机','正常'],['存储','正常']].map((v,i)=>{
  const y=186+i*39;
  return rr(40,y,400,34,9,'#141B28','#2A3345')+t(60,y+23,v[0],14,'#DCE5F5')+t(420,y+23,v[1],13,v[1]==='正常'?'#4DD0E1':'#FFC857','end','bold');
}).join('')}
${t(240,472,'检查完成后等待锁屏时间 / 天气就绪再淡出',12,'#7E8BA5','middle')}
`
  },
  {
    name: '13-wifi-scan',
    body: `
${bg('#0B0F18')}
${t(20,38,'附近网络',20,'#FFFFFF','start','bold')}${t(460,38,'扫描完成',12,'#22D3EE','end')}
${rr(12,58,456,370,16,'#14161E','#2A3345')}
${t(30,88,'已保存',13,'#8E9BB5')}
${[['MyWiFi','已连接 · 192.168.1.x'],['TP-LINK_5G','已保存']].map((v,i)=>{
  const y=98+i*50;
  return rr(26,y,428,44,10,'#1B2334','#2A3345')+t(42,y+28,v[0],14,'#DCE5F5')+t(300,y+28,v[1],12,i===0?'#22D3EE':'#8E9BB5')+(i>0?rr(370,y+9,70,26,13,'#3A1F2B','#7C4DFF')+t(405,y+27,'忘记',12,'#FF8FA3','middle'):'');
}).join('')}
${t(30,220,'附近',13,'#8E9BB5')}
${[['CMCC-8f2a','-52 dBm'],['ChinaNet-x7','-68 dBm'],['MyWiFi','-71 dBm']].map((v,i)=>{
  const y=230+i*50;
  return rr(26,y,428,44,10,'#1B2334','#2A3345')+t(42,y+28,v[0],14,'#DCE5F5')+t(420,y+28,v[1],12,'#8E9BB5','end');
}).join('')}
${t(240,447,'点行连接；已保存网络可「忘记」，避免自动回连',12,'#7E8BA5','middle')}
${rr(60,452,360,26,13,'url(#purple)')}${t(240,470,'关闭',13,'#FFFFFF','middle')}
`
  }
];

for (const p of panels) {
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="480" height="480" viewBox="0 0 480 480"><defs>${commonDefs}</defs>${p.body}</svg>`;
  fs.writeFileSync(`${dir}/${p.name}.svg`, svg, 'utf8');
  console.log('wrote', p.name);
}
