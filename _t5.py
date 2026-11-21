import urllib.request, os
T=os.environ['TEMP']
def get(u):
    req=urllib.request.Request(u, headers={'User-Agent':'Mozilla/5.0 (ESP32-P4C5)'})
    try: return urllib.request.urlopen(req,timeout=15).read()
    except Exception as e: return ('ERR '+str(e)).encode()
tests={
 'd180':'http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=sh000001,day,,,180,qfq',
 'w180':'http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=sh000001,week,,,180,qfq',
 'w60':'http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=sh000001,week,,,60,qfq',
 'm180':'http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=sh000001,month,,,180,qfq',
 'usd':'http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=us.DJI,day,,,180,qfq',
}
out=[]
for k,u in tests.items():
    d=get(u)
    t=d.decode('utf-8','ignore')
    out.append("%s len=%d | %s" % (k, len(d), t[:160].replace("\n"," ")))
open('_t5.log','w',encoding='utf-8').write("\n".join(out))
