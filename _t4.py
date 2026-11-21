import urllib.request, os
T=os.environ['TEMP']
def get(u):
    req=urllib.request.Request(u, headers={'User-Agent':'Mozilla/5.0'})
    try: return urllib.request.urlopen(req,timeout=15).read()
    except Exception as e: return ('ERR '+str(e)).encode()
tests={
 'usOQ_week':'http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=usAAPL.OQ,week,,,10,qfq',
 'usOQ_month':'http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=usAAPL.OQ,month,,,5,qfq',
 'usDJI':'http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=us.DJI,day,,,10,qfq',
 'usIXIC':'http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=us.IXIC,day,,,10,qfq',
 'usINX':'http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=us.INX,day,,,10,qfq',
 'a_month_qfq':'http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=sz002517,month,,,5,qfq',
}
out=[]
for k,u in tests.items():
    d=get(u)
    txt=d.decode('utf-8','ignore')
    out.append("=== %s len=%d\n%s" % (k, len(d), txt[:260].replace("\n"," ")))
open('_t4.log','w',encoding='utf-8').write("\n".join(out))
