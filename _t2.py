import urllib.request, os, json, traceback
T=os.environ['TEMP']
def get(u):
    req=urllib.request.Request(u, headers={'User-Agent':'Mozilla/5.0'})
    try: return urllib.request.urlopen(req,timeout=15).read()
    except Exception as e: return ('ERR '+str(e)).encode()
tests={
 'us_day':'http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=usAAPL,day,,,30,qfq',
 'us_week':'http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=usAAPL,week,,,10,qfq',
 'us_month':'http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=usAAPL,month,,,5,qfq',
 'idx_day':'http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=sh000001,day,,,5,qfq',
 'idx_month':'http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=sh000001,month,,,3,qfq',
}
out=[]
for k,u in tests.items():
    d=get(u).decode('utf-8','ignore')
    open(os.path.join(T,k+'.json'),'w',encoding='utf-8').write(d)
    out.append("%s len=%d %s" % (k, len(d), d[:200].replace("\n"," ")))
open('_t2.log','w',encoding='utf-8').write("\n".join(out))
