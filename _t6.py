import urllib.request, os, json
T=os.environ['TEMP']
def get(u):
    req=urllib.request.Request(u, headers={'User-Agent':'Mozilla/5.0'})
    try: return urllib.request.urlopen(req,timeout=15).read()
    except Exception as e: return ('ERR '+str(e)).encode()
def url(secid, klt, lmt=180):
    return ("http://push2his.eastmoney.com/api/qt/stock/kline/get?"
            "secid=%s&klt=%d&fqt=1&lmt=%d&end=20500101"
            "&fields1=f1,f2,f3,f4,f5,f6&fields2=f51,f52,f53,f54,f55,f56" % (secid, klt, lmt))
tests={
 'sh_day':url('1.000001',101),
 'sh_week':url('1.000001',102),
 'sh_month':url('1.000001',103),
 'aapl_day':url('105.AAPL',101),
 'aapl_month':url('105.AAPL',103),
 'dji_day':url('100.DJIA',101),
}
out=[]
for k,u in tests.items():
    d=get(u)
    t=d.decode('utf-8','ignore')
    out.append("%s len=%d | %s" % (k,len(d), t[:180].replace("\n"," ")))
open('_t6.log','w',encoding='utf-8').write("\n".join(out))
