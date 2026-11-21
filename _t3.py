import urllib.request, os
T=os.environ['TEMP']
def get(u, ref=None):
    h={'User-Agent':'Mozilla/5.0'}
    if ref: h['Referer']=ref
    req=urllib.request.Request(u, headers=h)
    try: return urllib.request.urlopen(req,timeout=15).read()
    except Exception as e: return ('ERR '+str(e)).encode()
tests={
 'us_oq':'http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=usAAPL.OQ,day,,,30,qfq',
 'us_date':'http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=usAAPL,day,2025-01-01,2026-11-20,320,qfq',
 'sina_us_k':'https://stock.finance.sina.com.cn/usstock/api/json_v2.php/US_MinKService.getDailyK?symbol=AAPL&___qn=3',
 'sina_us_q':'http://hq.sinajs.cn/list=gb_aapl',
 'week_a':'http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=sh600519,week,,,10,qfq',
}
out=[]
for k,u in tests.items():
    d=get(u, 'http://finance.sina.com.cn' if 'sina' in u else None)
    txt=d.decode('utf-8','ignore')
    open(os.path.join(T,k+'.txt'),'w',encoding='utf-8').write(txt)
    out.append("=== %s len=%d\n%s" % (k, len(d), txt[:300].replace("\n"," ")))
open('_t3.log','w',encoding='utf-8').write("\n".join(out))
