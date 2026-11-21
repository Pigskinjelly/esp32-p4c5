import urllib.request, os
T=os.environ['TEMP']
def get(u):
    req=urllib.request.Request(u, headers={'User-Agent':'Mozilla/5.0'})
    try: return urllib.request.urlopen(req,timeout=12).read().decode('utf-8','ignore')
    except Exception as e: return 'ERR '+str(e)
def probe(secid):
    u=("http://push2his.eastmoney.com/api/qt/stock/kline/get?secid=%s&klt=101&fqt=1&lmt=3"
       "&end=20500101&fields1=f1,f2&fields2=f51,f52,f53" % secid)
    t=get(u)
    ok='"klines"' in t
    name=''
    i=t.find('"name":"')
    if i>0: name=t[i+8:t.find('"', i+8)]
    return ok, name, t[:80]
out=[]
for sid in ['100.IXIC','100.NDX','100.SPX','100.INX','105.TSLA','106.BABA','105.NVDA','105.MSFT']:
    ok,name,head=probe(sid)
    out.append("%-10s ok=%s name=%s | %s" % (sid, ok, name, head.replace("\n"," ")))
open('_t7.log','w',encoding='utf-8').write("\n".join(out))
