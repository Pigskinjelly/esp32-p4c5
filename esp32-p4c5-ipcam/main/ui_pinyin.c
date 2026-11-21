/* 拼音 → 汉字表（自动生成，勿手改）
 * 数据来源：https://github.com/mozillazg/pinyin-data (MIT)
 * 字符集：与 ui_cjk_16 字体一致，保证每个候选都能显示
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "nvs.h"
#include "esp_log.h"
#include "ui_pinyin.h"

typedef struct { const char *py; const char *chars; } py_ent_t;

static const py_ent_t s_py[] = {
    { "a", "啊阿" },
    { "ai", "哀哎唉埃挨爱癌皑矮碍艾蔼隘" },
    { "an", "俺安岸按暗案氨胺鞍" },
    { "ang", "昂盎肮" },
    { "ao", "傲凹奥懊敖澳熬翱袄" },
    { "ba", "八叭吧坝巴扒把拔捌爸疤笆罢耙芭跋霸靶" },
    { "bai", "佰拜摆柏白百稗败" },
    { "ban", "伴办半扮扳拌搬斑板版班瓣绊般颁" },
    { "bang", "傍帮梆棒榜磅绑膀蚌谤邦镑" },
    { "bao", "保包堡宝报抱暴爆胞苞薄褒豹雹饱鲍" },
    { "bei", "倍北卑备悲惫杯焙狈碑背被贝辈钡" },
    { "ben", "奔本笨苯" },
    { "beng", "崩泵甭绷蹦迸" },
    { "bi", "壁币庇弊彼必敝比毕毖毙痹碧笔臂蓖蔽逼避鄙闭陛鼻" },
    { "bian", "便卞变扁编贬辨辩辫边遍鞭" },
    { "biao", "彪标膘表" },
    { "bie", "别憋瘪鳖" },
    { "bin", "宾彬摈斌滨濒" },
    { "bing", "丙兵冰并柄炳病秉饼" },
    { "bo", "伯剥勃博卜帛拨搏播波渤玻箔脖膊舶菠钵铂驳" },
    { "bu", "不哺埠布怖捕步簿补部" },
    { "ca", "擦" },
    { "cai", "彩才材猜睬菜蔡裁财踩采" },
    { "can", "参惨惭掺残灿蚕餐" },
    { "cang", "仓沧舱苍藏" },
    { "cao", "操曹槽糙草" },
    { "ce", "侧册厕测策" },
    { "ceng", "层曾蹭" },
    { "cha", "叉察岔差插搽查碴茬茶诧" },
    { "chai", "拆柴豺" },
    { "chan", "产搀缠蝉谗铲阐颤馋" },
    { "chang", "倡偿厂唱场尝常敞昌猖畅肠" },
    { "chao", "吵嘲巢抄朝潮炒超钞" },
    { "che", "彻扯掣撤澈车" },
    { "chen", "尘忱晨沉臣衬趁辰郴陈" },
    { "cheng", "乘呈城惩成承撑橙澄秤称程诚逞骋" },
    { "chi", "侈吃尺弛持斥池炽痴翅耻赤迟驰齿" },
    { "chong", "充冲宠崇虫" },
    { "chou", "丑仇愁抽畴瞅稠筹绸臭踌酬" },
    { "chu", "储出初厨处搐楚橱滁畜矗础触躇锄除雏" },
    { "chuai", "揣" },
    { "chuan", "串传喘川椽穿船" },
    { "chuang", "创幢床疮窗闯" },
    { "chui", "吹垂捶椎炊锤" },
    { "chun", "唇春椿淳纯蠢醇" },
    { "chuo", "戳绰" },
    { "ci", "伺刺慈次此瓷疵磁茨词赐辞雌" },
    { "cong", "丛从匆囱聪葱" },
    { "cou", "凑" },
    { "cu", "促簇粗醋" },
    { "cuan", "窜篡蹿" },
    { "cui", "催崔摧淬瘁粹翠脆" },
    { "cun", "存寸村" },
    { "cuo", "挫措搓撮磋错" },
    { "da", "大打搭瘩答达" },
    { "dai", "代傣呆带待怠戴歹殆袋贷逮" },
    { "dan", "丹但单弹惮担掸旦氮淡耽胆蛋诞郸" },
    { "dang", "党当挡档荡" },
    { "dao", "倒刀到导岛悼捣盗祷稻蹈道" },
    { "de", "得德的" },
    { "deng", "凳灯登瞪等蹬邓" },
    { "di", "低地堤嫡帝底弟抵敌涤滴狄笛第缔翟蒂迪递" },
    { "dian", "佃典垫奠店惦掂殿淀滇点电甸碘靛颠" },
    { "diao", "凋刁叼吊掉碉调钓雕" },
    { "die", "叠爹碟蝶谍跌迭" },
    { "ding", "丁叮定盯订钉锭顶鼎" },
    { "diu", "丢" },
    { "dong", "东侗冬冻动恫懂栋洞董" },
    { "dou", "兜抖斗痘豆逗都陡" },
    { "du", "堵妒度杜毒渡犊独督睹肚读赌镀" },
    { "duan", "断段短端缎锻" },
    { "dui", "兑堆对队" },
    { "dun", "吨囤墩敦盾蹲遁钝顿" },
    { "duo", "剁哆垛堕多夺惰掇朵舵跺躲" },
    { "e", "俄厄娥峨恶扼蛾讹遏鄂额饿鹅" },
    { "en", "恩" },
    { "er", "二儿尔洱而耳贰饵" },
    { "fa", "乏伐发法珐筏罚阀" },
    { "fan", "凡反帆樊泛烦犯番矾繁翻范藩贩返钒饭" },
    { "fang", "仿坊妨房放方纺肪芳访防" },
    { "fei", "匪吠啡废沸肥肺菲诽费非飞" },
    { "fen", "份分吩坟奋忿愤氛汾焚粉粪纷芬酚" },
    { "feng", "丰冯凤奉封峰枫烽疯缝蜂讽逢锋风" },
    { "fou", "否" },
    { "fu", "付伏佛俘俯傅副咐复夫妇孵富幅府弗扶抚拂敷斧服氟浮涪父甫福符缚肤腐腑腹袱覆讣负赋赴辅辐釜阜附" },
    { "ga", "嘎噶" },
    { "gai", "改概溉盖该钙" },
    { "gan", "干感敢杆柑甘秆竿肝赣赶" },
    { "gang", "冈刚岗杠港纲缸肛钢" },
    { "gao", "告搞皋稿篙糕羔膏镐高" },
    { "ge", "个割各咯哥戈搁格歌疙胳葛铬阁隔革鸽" },
    { "gei", "给" },
    { "gen", "根跟" },
    { "geng", "埂庚更梗羹耕耿" },
    { "gong", "供公共功宫工巩弓恭拱攻汞贡躬龚" },
    { "gou", "勾垢够构沟狗苟购钩" },
    { "gu", "估古咕固姑孤故沽箍股菇蛊谷辜雇顾骨鼓" },
    { "gua", "刮剐寡挂瓜褂" },
    { "guai", "乖怪拐" },
    { "guan", "关冠官惯棺灌管罐观贯馆" },
    { "guang", "光广逛" },
    { "gui", "傀刽圭归柜桂瑰癸硅规诡贵跪轨闺鬼龟" },
    { "gun", "棍滚辊" },
    { "guo", "国果裹过郭锅" },
    { "ha", "哈蛤" },
    { "hai", "亥孩害氦海还骇骸" },
    { "han", "函含喊寒悍憨憾捍撼旱汉汗涵焊罕翰邯酣韩" },
    { "hang", "夯杭航" },
    { "hao", "号嚎壕好毫浩耗豪貉郝" },
    { "he", "何合呵和喝核河涸盒禾荷菏褐贺赫阂鹤" },
    { "hei", "嘿黑" },
    { "hen", "很恨狠痕" },
    { "heng", "亨哼恒横衡" },
    { "hong", "哄宏弘洪烘红虹轰鸿" },
    { "hou", "侯候厚后吼喉猴" },
    { "hu", "乎互呼唬壶弧忽户护沪湖狐瑚糊胡葫虎蝴" },
    { "hua", "划化华哗滑猾画花话" },
    { "huai", "坏徊怀槐淮" },
    { "huan", "唤宦幻患换桓欢涣焕环痪缓豢" },
    { "huang", "凰幌恍惶慌晃煌皇磺簧荒蝗谎黄" },
    { "hui", "会卉回徽恢悔惠慧挥晦毁汇灰烩秽绘蛔讳诲贿辉" },
    { "hun", "婚昏浑混荤魂" },
    { "huo", "伙惑或活火祸获豁货霍" },
    { "ji", "伎冀几击剂即及吉圾基妓姬嫉季寂寄己忌急悸技挤既机极棘汲济激畸疾祭积稽箕籍级纪继绩缉肌脊蓟藉计讥记辑迹际集饥鸡" },
    { "jia", "价佳假加嘉夹嫁家架枷甲稼茄荚贾钾颊驾" },
    { "jian", "件俭健兼减剑剪坚奸尖建拣捡柬检歼涧渐溅煎监硷碱笺简箭缄肩舰艰茧荐见贱践鉴键间饯" },
    { "jiang", "僵匠奖姜将桨江浆疆蒋讲酱降" },
    { "jiao", "交侥剿叫娇搅教椒浇焦狡矫礁窖绞缴胶脚蕉角轿较郊酵铰饺骄" },
    { "jie", "介借劫姐届戒截捷接揭杰洁界疥皆睫秸竭结节芥街解诫阶" },
    { "jin", "仅今劲尽巾斤晋津浸烬禁筋紧襟谨近进金锦靳" },
    { "jing", "井京兢净境径惊敬景晶痉睛竞竟粳精经茎荆警镜靖静颈鲸" },
    { "jiong", "炯窘" },
    { "jiu", "久九厩咎就揪救旧灸玖疚究纠臼舅酒韭" },
    { "ju", "举俱具剧句咀局居巨惧拒拘据桔沮炬狙疽矩聚菊距踞锯鞠驹" },
    { "juan", "倦卷娟捐眷绢鹃" },
    { "jue", "倔决嚼抉掘撅攫爵绝觉诀" },
    { "jun", "俊军君均峻浚竣菌郡钧骏" },
    { "ka", "卡咖喀" },
    { "kai", "凯开恺慨揩楷" },
    { "kan", "刊勘坎堪槛看砍" },
    { "kang", "亢康慷扛抗炕糠" },
    { "kao", "拷烤考靠" },
    { "ke", "克刻可咳坷壳客柯棵渴磕科苛课颗" },
    { "ken", "啃垦恳肯" },
    { "keng", "吭坑" },
    { "kong", "孔恐控空" },
    { "kou", "口寇扣抠" },
    { "ku", "哭库枯窟苦裤酷" },
    { "kua", "垮夸挎胯跨" },
    { "kuai", "侩块快筷" },
    { "kuan", "宽款" },
    { "kuang", "况匡旷框狂眶矿筐" },
    { "kui", "亏奎岿愧溃盔窥葵馈魁" },
    { "kun", "困坤捆昆" },
    { "kuo", "廓扩括阔" },
    { "la", "啦喇垃拉腊蜡辣" },
    { "lai", "来莱赖" },
    { "lan", "兰婪懒拦揽栏滥澜烂篮缆蓝览谰阑" },
    { "lang", "廊朗榔浪狼琅郎" },
    { "lao", "佬劳姥捞涝潦烙牢老酪" },
    { "le", "乐了肋" },
    { "lei", "儡勒垒擂泪磊类累蕾镭雷" },
    { "leng", "冷棱楞" },
    { "li", "丽例俐傈利力励历厉厘吏哩李栗梨沥漓犁狸理璃痢砾礼离立篱粒荔莉里隶鲤黎" },
    { "lia", "俩" },
    { "lian", "帘廉怜恋敛涟炼练联脸莲连链镰" },
    { "liang", "两亮凉晾梁粮粱良谅辆量" },
    { "liao", "僚寥廖撂撩料燎疗聊辽镣" },
    { "lie", "列劣烈猎裂" },
    { "lin", "临凛吝拎林淋琳磷赁邻霖鳞" },
    { "ling", "令伶凌另岭灵玲羚菱铃陵零领龄" },
    { "liu", "六刘柳榴流溜琉留瘤硫馏" },
    { "long", "咙垄拢窿笼聋陇隆龙" },
    { "lou", "娄搂楼漏篓陋" },
    { "lu", "卢卤庐录戮掳潞炉碌禄芦虏赂路陆露颅鲁鹿麓" },
    { "luan", "乱卵孪峦挛滦" },
    { "lun", "仑伦抡沦纶论轮" },
    { "luo", "洛箩络罗萝落螺裸逻锣骆骡" },
    { "lv", "侣吕屡履律旅氯滤率绿缕虑铝驴" },
    { "lve", "掠略" },
    { "ma", "吗嘛妈玛码蚂马骂麻" },
    { "mai", "买卖埋脉迈麦" },
    { "man", "慢曼满漫瞒蔓蛮谩馒" },
    { "mang", "忙氓盲芒茫莽" },
    { "mao", "冒卯帽毛猫矛茂茅貌贸铆锚" },
    { "me", "么" },
    { "mei", "妹媒媚寐昧枚梅每没煤玫眉美酶镁霉" },
    { "men", "们门闷" },
    { "meng", "孟梦檬猛盟萌蒙锰" },
    { "mi", "密幂弥泌眯秘米糜蜜觅谜迷醚靡" },
    { "mian", "免冕勉娩棉眠绵缅面" },
    { "miao", "妙庙描渺瞄秒苗藐" },
    { "mie", "灭蔑" },
    { "min", "悯抿敏民皿闽" },
    { "ming", "名命明螟铭鸣" },
    { "miu", "谬" },
    { "mo", "墨寞抹摩摸摹末模沫漠磨膜莫蘑陌魔默" },
    { "mou", "某牟谋" },
    { "mu", "亩募墓姆幕慕拇暮木母牡牧目睦穆" },
    { "na", "呐哪娜拿纳那钠" },
    { "nai", "乃奈奶氖耐" },
    { "nan", "南男难" },
    { "nang", "囊" },
    { "nao", "恼挠淖脑闹" },
    { "ne", "呢" },
    { "nei", "内馁" },
    { "nen", "嫩" },
    { "neng", "能" },
    { "ni", "你倪匿妮尼拟昵泥溺腻逆霓" },
    { "nian", "年念拈捻撵碾蔫辗" },
    { "niang", "娘酿" },
    { "niao", "尿鸟" },
    { "nie", "啮孽捏涅聂镊镍" },
    { "nin", "您" },
    { "ning", "凝宁拧柠泞狞" },
    { "niu", "扭牛纽钮" },
    { "nong", "农弄浓脓" },
    { "nu", "努奴怒" },
    { "nuan", "暖" },
    { "nuo", "懦挪糯诺" },
    { "nv", "女" },
    { "nve", "疟虐" },
    { "o", "哦" },
    { "ou", "偶呕欧殴沤藕鸥" },
    { "pa", "啪帕怕爬琶趴" },
    { "pai", "徘拍排派湃牌" },
    { "pan", "判叛攀潘畔盘盼磐" },
    { "pang", "乓庞旁耪胖" },
    { "pao", "刨咆抛泡炮袍跑" },
    { "pei", "佩呸培沛胚裴赔配陪" },
    { "pen", "喷盆" },
    { "peng", "彭抨捧朋棚澎烹砰硼碰篷膨蓬鹏" },
    { "pi", "僻劈匹啤坯屁批披毗琵疲痞皮砒脾譬辟霹" },
    { "pian", "偏片篇骗" },
    { "piao", "漂瓢票飘" },
    { "pie", "撇瞥" },
    { "pin", "品拼聘贫频" },
    { "ping", "乒凭坪屏平瓶苹萍评" },
    { "po", "坡婆泊泼破粕迫颇魄" },
    { "pou", "剖" },
    { "pu", "仆圃埔扑普曝朴浦瀑脯莆菩葡蒲谱铺" },
    { "qi", "七乞企其凄启器奇契妻岂崎弃戚旗期柒栖棋欺歧气汽沏泣漆畦砌祁祈脐讫起迄骑齐" },
    { "qia", "恰掐洽" },
    { "qian", "乾仟前千堑嵌扦欠歉浅潜牵签谦谴迁遣钎钱钳铅黔" },
    { "qiang", "呛墙强抢枪羌腔蔷" },
    { "qiao", "乔侨俏峭巧悄撬敲桥橇瞧窍翘锹鞘" },
    { "qie", "且切怯窃" },
    { "qin", "亲侵勤寝擒沁琴禽秦芹钦" },
    { "qing", "倾卿庆情擎晴氢氰清请轻青顷" },
    { "qiong", "琼穷" },
    { "qiu", "丘囚求泅球秋邱酋" },
    { "qu", "区去取娶屈曲渠蛆趋趣躯驱龋" },
    { "quan", "全券劝圈拳权泉犬痊醛颧" },
    { "que", "却榷炔瘸确缺雀鹊" },
    { "qun", "群裙" },
    { "ran", "冉染然燃" },
    { "rang", "嚷壤攘瓤让" },
    { "rao", "扰绕饶" },
    { "re", "惹热" },
    { "ren", "人仁任刃壬妊忍纫认韧" },
    { "reng", "仍扔" },
    { "ri", "日" },
    { "rong", "冗容戎溶熔绒茸荣蓉融" },
    { "rou", "揉柔肉" },
    { "ru", "乳儒入如孺汝茹蠕褥辱" },
    { "ruan", "软阮" },
    { "rui", "瑞蕊锐" },
    { "run", "润闰" },
    { "ruo", "弱若" },
    { "sa", "撒洒萨" },
    { "sai", "塞腮赛鳃" },
    { "san", "三伞叁散" },
    { "sang", "丧嗓桑" },
    { "sao", "嫂扫搔骚" },
    { "se", "涩瑟色" },
    { "sen", "森" },
    { "seng", "僧" },
    { "sha", "傻刹厦啥杀沙煞砂纱莎" },
    { "shai", "晒筛" },
    { "shan", "删善山扇擅杉汕煽珊缮膳苫衫赡闪陕" },
    { "shang", "上伤商墒尚晌裳赏" },
    { "shao", "勺哨少捎梢烧稍绍芍邵韶" },
    { "she", "奢射慑摄涉社舌舍蛇设赊赦" },
    { "shen", "什伸呻娠婶审慎沈深渗甚申砷神绅肾身" },
    { "sheng", "剩升圣声牲生甥盛省绳胜" },
    { "shi", "世事仕似使侍势匙十史嗜噬士失始实室尸屎市师式恃拭拾施时是柿氏湿狮矢石示虱蚀视誓识试诗适逝释食饰驶" },
    { "shou", "兽受售守寿手授收瘦首" },
    { "shu", "书叔墅孰属庶恕戍抒数暑曙术束枢树梳殊淑漱熟疏竖署舒蔬薯蜀赎输述黍鼠" },
    { "shua", "刷耍" },
    { "shuai", "帅摔甩衰" },
    { "shuan", "拴栓" },
    { "shuang", "双爽霜" },
    { "shui", "水睡税谁" },
    { "shun", "吮瞬舜顺" },
    { "shuo", "朔烁硕说" },
    { "si", "丝司嗣嘶四寺巳思撕斯死私肆饲" },
    { "song", "宋怂松耸讼诵送颂" },
    { "sou", "嗽搜擞艘" },
    { "su", "俗僳塑宿溯粟素肃苏诉速酥" },
    { "suan", "算蒜酸" },
    { "sui", "岁碎祟穗绥虽遂隋随隧髓" },
    { "sun", "孙损笋" },
    { "suo", "唆所梭琐索缩蓑锁" },
    { "ta", "他塌塔她它挞獭踏蹋" },
    { "tai", "台太态抬汰泰胎苔酞" },
    { "tan", "叹坍坛坦探摊檀毯滩潭炭痰瘫碳袒谈谭贪" },
    { "tang", "倘唐堂塘搪棠汤淌烫糖膛趟躺" },
    { "tao", "套掏桃涛淘滔绦萄讨逃陶" },
    { "te", "特" },
    { "teng", "疼腾藤誊" },
    { "ti", "体剃剔啼嚏屉惕提替梯涕踢蹄锑题" },
    { "tian", "填天恬添甜田腆舔" },
    { "tiao", "挑条眺跳迢" },
    { "tie", "帖贴铁" },
    { "ting", "亭停厅听庭廷挺汀烃艇" },
    { "tong", "同彤捅桐桶痛瞳童筒统通酮铜" },
    { "tou", "偷头投透" },
    { "tu", "兔凸吐图土屠徒涂秃突途" },
    { "tuan", "团湍" },
    { "tui", "推腿蜕褪退颓" },
    { "tun", "吞屯臀" },
    { "tuo", "唾妥托拓拖椭脱陀驮驼鸵" },
    { "wa", "哇娃挖洼瓦蛙袜" },
    { "wai", "外歪" },
    { "wan", "万丸婉完宛弯惋挽晚湾烷玩皖碗腕豌顽" },
    { "wang", "亡妄往忘旺望枉汪王网" },
    { "wei", "为伟伪位卫危味唯喂围委威尉尾巍微惟慰未桅渭潍畏纬维胃苇萎蔚谓违韦魏" },
    { "wen", "吻文温瘟稳紊纹蚊问闻" },
    { "weng", "嗡瓮翁" },
    { "wo", "卧我挝握斡沃涡窝蜗" },
    { "wu", "乌五伍侮务勿午吴吾呜坞屋巫悟戊捂无晤梧武毋污物舞芜诬误钨雾" },
    { "xi", "习吸喜嘻夕媳希席息悉惜戏昔晰析檄汐洗溪烯熄熙牺犀矽硒稀系细膝袭西铣锡隙" },
    { "xia", "下侠匣吓夏峡暇狭瞎虾辖霞" },
    { "xian", "仙先县咸嫌宪弦掀显涎献现纤线羡腺舷衔贤锨闲限险陷馅鲜" },
    { "xiang", "乡享像厢向响巷想橡湘相祥箱翔襄详象镶项香" },
    { "xiao", "哮啸嚣孝宵小效晓校消淆硝笑肖萧销霄" },
    { "xie", "些写协卸屑懈挟携斜械楔歇泄泻胁蝎蟹谐谢邪鞋" },
    { "xin", "信心忻新欣芯薪衅辛锌" },
    { "xing", "兴刑型姓幸形性惺星杏猩腥行邢醒" },
    { "xiong", "兄凶匈汹熊胸雄" },
    { "xiu", "休修嗅朽秀绣羞袖锈" },
    { "xu", "叙吁嘘墟婿序徐恤戌旭絮绪续蓄虚许酗需须" },
    { "xuan", "喧宣悬旋玄癣眩绚轩选" },
    { "xue", "削学穴薛血雪靴" },
    { "xun", "勋寻巡循旬殉汛熏训讯询迅逊驯" },
    { "ya", "丫亚压呀哑崖押涯牙芽蚜衙讶轧雅鸦鸭" },
    { "yan", "严厌咽唁堰奄宴岩延彦掩沿淹演炎烟焉焰燕盐眼研砚艳蜒衍言谚阉阎雁颜验" },
    { "yang", "仰佯养央扬杨样殃氧洋漾疡痒秧羊阳鸯" },
    { "yao", "咬妖姚尧摇瑶窑耀腰舀药要谣遥邀钥" },
    { "ye", "业也冶叶噎夜掖曳椰液爷耶腋野页" },
    { "yi", "一义乙亦亿以仪伊依倚医壹夷姨宜屹已异彝役忆意抑揖易椅毅沂溢疑疫益矣移绎翌翼肄胰臆艺蚁衣裔议译诣谊逸遗邑铱颐驿" },
    { "yin", "印吟因姻寅尹引殷淫茵荫银阴隐音饮" },
    { "ying", "婴应影映樱盈硬缨英荧莹萤营蝇赢迎颖鹰" },
    { "yo", "哟" },
    { "yong", "佣勇咏庸恿拥永泳涌用痈臃蛹踊雍" },
    { "you", "优佑又友右尤幼幽忧悠有油游犹由诱邮酉釉铀" },
    { "yu", "与予于余俞喻域娱宇寓屿峪御愈愉愚榆欲浴淤渔渝狱玉盂禹羽育舆芋虞裕誉语豫迂逾遇郁隅雨预驭鱼" },
    { "yuan", "元冤原员园圆垣怨愿援渊源猿缘苑袁辕远院鸳" },
    { "yue", "岳悦曰月粤约越跃阅" },
    { "yun", "云允匀孕晕耘蕴运郧酝陨韵" },
    { "za", "匝咋杂砸" },
    { "zai", "仔再哉在宰栽灾载" },
    { "zan", "咱攒暂赞" },
    { "zang", "脏葬赃" },
    { "zao", "凿噪早枣澡灶燥皂糟藻蚤躁造遭" },
    { "ze", "则择泽责" },
    { "zei", "贼" },
    { "zen", "怎" },
    { "zeng", "增憎赠" },
    { "zha", "乍喳扎札柞栅榨渣炸眨诈铡闸" },
    { "zhai", "债宅寨摘斋窄" },
    { "zhan", "占展崭战斩栈毡沾湛盏瞻站粘绽蘸詹" },
    { "zhang", "丈仗帐张彰掌杖樟涨漳瘴章胀账长障" },
    { "zhao", "兆召找招昭沼照爪罩肇赵" },
    { "zhe", "哲折浙着者蔗蛰辙这遮锗" },
    { "zhen", "侦圳帧振斟枕珍甄疹真砧臻诊贞针镇阵震" },
    { "zheng", "争征怔拯挣政整正狰症睁蒸证郑" },
    { "zhi", "之侄值制只吱址峙帜志执指挚掷支旨智枝植止殖汁治滞炙痔直知秩稚窒纸织置职肢脂至致芝蜘质趾" },
    { "zhong", "中仲众忠盅种终肿衷重钟" },
    { "zhou", "周咒宙州帚昼洲皱粥肘舟诌轴骤" },
    { "zhu", "主住助嘱拄朱柱株注烛煮猪珠瞩祝竹筑著蛀蛛诛诸贮逐铸驻" },
    { "zhua", "抓" },
    { "zhuai", "拽" },
    { "zhuan", "专撰砖篆赚转" },
    { "zhuang", "壮妆庄撞桩状装" },
    { "zhui", "坠缀赘追锥" },
    { "zhun", "准谆" },
    { "zhuo", "卓啄拙捉桌浊灼茁酌" },
    { "zi", "兹咨姿子字孜淄渍滋滓籽紫自资" },
    { "zong", "宗总棕纵综踪鬃" },
    { "zou", "奏揍走邹" },
    { "zu", "卒族祖租组诅足阻" },
    { "zuan", "纂钻" },
    { "zui", "嘴最罪醉" },
    { "zun", "尊遵" },
    { "zuo", "佐作做坐左座昨琢" },
};

#define PY_CNT ((int)(sizeof(s_py)/sizeof(s_py[0])))

const char *ui_pinyin_chars(const char *py)
{
    if (!py || !py[0]) {
        return NULL;
    }
    size_t n = strlen(py);
    for (int i = 0; i < PY_CNT; i++) {
        if (strncmp(s_py[i].py, py, n) == 0) {
            return s_py[i].chars;
        }
    }
    return NULL;
}

int ui_pinyin_is_syllable(const char *py)
{
    if (!py || !py[0]) {
        return 0;
    }
    for (int i = 0; i < PY_CNT; i++) {
        if (strcmp(s_py[i].py, py) == 0) {
            return 1;
        }
    }
    return 0;
}

const char *ui_pinyin_first(const char *py)
{
    const char *c = ui_pinyin_chars(py);
    if (!c || !c[0]) {
        return NULL;
    }
    static char buf[8];
    int n = ((unsigned char)c[0] < 0x80) ? 1 : (((unsigned char)c[0] & 0xF0) == 0xE0 ? 3 : 2);
    memcpy(buf, c, (size_t)n);
    buf[n] = 0;
    return buf;
}

/* ================= 常用词库 =================
 * 只放高频双字/三字词，打前半截就能整词上屏（比逐字选快很多）。
 * 字符集必须落在 ui_cjk_16 里，否则会显示方框。
 */
typedef struct { const char *py; const char *word; } py_word_t;

static const py_word_t s_words[] = {
    /* 人称 / 打招呼 */
    { "nihao", "你好" },       { "ninhao", "您好" },     { "women", "我们" },
    { "nimen", "你们" },       { "tamen", "他们" },      { "xiexie", "谢谢" },
    { "zaijian", "再见" },     { "duibuqi", "对不起" },  { "meiguanxi", "没关系" },
    { "qingwen", "请问" },     { "zaoshanghao", "早上好" }, { "wanan", "晚安" },
    { "haode", "好的" },       { "shide", "是的" },      { "bushi", "不是" },
    { "buyong", "不用" },      { "xinku", "辛苦" },      { "huanying", "欢迎" },
    /* 疑问 / 连接 */
    { "shenme", "什么" },      { "zenme", "怎么" },      { "weishenme", "为什么" },
    { "zenmeyang", "怎么样" }, { "shenmeshihou", "什么时候" }, { "nali", "哪里" },
    { "zheli", "这里" },       { "duoshao", "多少" },    { "yisi", "意思" },
    { "yinwei", "因为" },      { "suoyi", "所以" },      { "danshi", "但是" },
    { "erqie", "而且" },       { "ruguo", "如果" },      { "suiran", "虽然" },
    { "ranhou", "然后" },      { "huozhe", "或者" },     { "haishi", "还是" },
    { "yiqi", "一起" },        { "yixie", "一些" },      { "yiding", "一定" },
    /* 时间 */
    { "jintian", "今天" },     { "mingtian", "明天" },   { "zuotian", "昨天" },
    { "xianzai", "现在" },     { "shihou", "时候" },     { "shijian", "时间" },
    { "jinnian", "今年" },     { "qunian", "去年" },     { "mingnian", "明年" },
    { "meitian", "每天" },     { "zaoshang", "早上" },   { "zhongwu", "中午" },
    { "xiawu", "下午" },       { "wanshang", "晚上" },   { "xingqi", "星期" },
    { "yuefen", "月份" },      { "xiaoshi", "小时" },    { "fenzhong", "分钟" },
    { "miaozhong", "秒钟" },   { "zuijin", "最近" },     { "yijing", "已经" },
    { "zhengzai", "正在" },    { "kaishi", "开始" },     { "jieshu", "结束" },
    /* 生活 */
    { "chifan", "吃饭" },      { "shuijiao", "睡觉" },   { "gongzuo", "工作" },
    { "xuexi", "学习" },       { "xuexiao", "学校" },    { "laoshi", "老师" },
    { "xuesheng", "学生" },    { "pengyou", "朋友" },    { "dongxi", "东西" },
    { "shiqing", "事情" },     { "difang", "地方" },     { "banfa", "办法" },
    { "fangfa", "方法" },      { "zhuyi", "注意" },      { "anquan", "安全" },
    { "shengyin", "声音" },    { "zhaopian", "照片" },   { "xiangji", "相机" },
    { "dianhua", "电话" },     { "shouji", "手机" },     { "diannao", "电脑" },
    { "xiaoxi", "消息" },      { "xinwen", "新闻" },     { "shipin", "视频" },
    { "dianying", "电影" },    { "youxi", "游戏" },      { "tupian", "图片" },
    { "wenjian", "文件" },     { "shuju", "数据" },      { "xinxi", "信息" },
    { "neirong", "内容" },     { "jieguo", "结果" },     { "lishi", "历史" },
    { "jilu", "记录" },        { "liebiao", "列表" },    { "tongji", "统计" },
    { "fenxi", "分析" },       { "ceshi", "测试" },      { "wenti", "问题" },
    /* 系统 / 网络 */
    { "shezhi", "设置" },      { "xitong", "系统" },     { "chengxu", "程序" },
    { "yingyong", "应用" },    { "gongneng", "功能" },   { "xuanze", "选择" },
    { "shuru", "输入" },       { "xianshi", "显示" },    { "queding", "确定" },
    { "quxiao", "取消" },      { "fanhui", "返回" },     { "shouye", "首页" },
    { "zhuye", "主页" },       { "guanli", "管理" },     { "quanxian", "权限" },
    { "yonghu", "用户" },      { "zhanghao", "账号" },   { "denglu", "登录" },
    { "zhuce", "注册" },       { "wangluo", "网络" },    { "lianjie", "连接" },
    { "duankai", "断开" },     { "saomiao", "扫描" },    { "mima", "密码" },
    { "wuxian", "无线" },      { "xinhao", "信号" },     { "dianchi", "电池" },
    { "chongdian", "充电" },   { "shengdian", "省电" },  { "liangdu", "亮度" },
    { "yinliang", "音量" },    { "jingyin", "静音" },    { "lanya", "蓝牙" },
    { "daiji", "待机" },       { "chongqi", "重启" },    { "kaiji", "开机" },
    { "guanji", "关机" },      { "gengxin", "更新" },    { "banben", "版本" },
    { "huifu", "恢复" },       { "moren", "默认" },
    { "chakan", "查看" },      { "shuaxin", "刷新" },    { "jiazai", "加载" },
    { "baocun", "保存" },      { "shanchu", "删除" },    { "xiugai", "修改" },
    { "xiazai", "下载" },      { "shangchuan", "上传" }, { "sousuo", "搜索" },
    { "huancun", "缓存" },     { "liuliang", "流量" },   { "redian", "热点" },
    { "luyouqi", "路由器" },   { "neicun", "内存" },     { "cunchu", "存储" },
    { "kongjian", "空间" },    { "shengyu", "剩余" },    { "sudu", "速度" },
    /* 音乐 */
    { "yinyue", "音乐" },      { "gequ", "歌曲" },       { "bofang", "播放" },
    { "zanting", "暂停" },     { "xiayishou", "下一首" }, { "shangyishou", "上一首" },
    { "shoucang", "收藏" },    { "xihuan", "喜欢" },     { "geshou", "歌手" },
    { "zhuanji", "专辑" },     { "geci", "歌词" },       { "diantai", "电台" },
    { "tuijian", "推荐" },     { "remen", "热门" },      { "paihang", "排行" },
    { "zaixian", "在线" },     { "lixian", "离线" },
    /* 地图 / 出行 */
    { "ditu", "地图" },        { "daohang", "导航" },    { "weizhi", "位置" },
    { "daoda", "到达" },       { "chufa", "出发" },      { "luduan", "路段" },
    { "juli", "距离" },        { "luxian", "路线" },
    /* 天气 */
    { "tianqi", "天气" },      { "wendu", "温度" },      { "shidu", "湿度" },
    { "fengli", "风力" },      { "qingtian", "晴天" },   { "xiayu", "下雨" },
    { "xiaxue", "下雪" },      { "duoyun", "多云" },     { "yintian", "阴天" },
    { "dafeng", "大风" },      { "gaowen", "高温" },     { "kongqi", "空气" },
    /* 监控 / 拍照 */
    { "jiankong", "监控" },    { "xiangce", "相册" },    { "paizhao", "拍照" },
    { "luxiang", "录像" },     { "huafu", "画幅" },      { "lianxu", "连续" },
    /* 股票 */
    { "gupiao", "股票" },      { "jiaoyi", "交易" },     { "shichang", "市场" },
    { "jiage", "价格" },       { "zhangfu", "涨幅" },    { "diefu", "跌幅" },
    { "chengjiao", "成交" },   { "zixuan", "自选" },
    /* 地名 / 常用名词 */
    { "zhongguo", "中国" },    { "beijing", "北京" },     { "shanghai", "上海" },
    { "shenzhen", "深圳" },    { "guangzhou", "广州" },   { "hangzhou", "杭州" },
    { "chengdu", "成都" },     { "wuhan", "武汉" },       { "nanjing", "南京" },
    { "chengshi", "城市" },    { "dizhi", "地址" },       { "dianyingyuan", "电影院" },
    { "gongyuan", "公园" },    { "yiyuan", "医院" },      { "canting", "餐厅" },
    /* 输入法 */
    { "hanzi", "汉字" },       { "pinyin", "拼音" },     { "shurufa", "输入法" },
    { "zhongwen", "中文" },    { "yingwen", "英文" },    { "yuyan", "语言" },
    { "fanyi", "翻译" },       { "juzi", "句子" },       { "ciyu", "词语" },
    { "zidian", "字典" },
    /* 歌名 / 歌手（搜索用，都是常用汉字） */
    { "tianwailaiwu", "天外来物" }, { "yanyuan", "演员" },   { "daoxiang", "稻香" },
    { "gaobaiqiqiu", "告白气球" }, { "qifengle", "起风了" },
    { "huayuan", "花园" },     { "wohewodezuguo", "我和我的祖国" },
    { "xuezhijian", "薛之谦" }, { "zhoujielun", "周杰伦" },  { "zhanghuimei", "张惠妹" },
    { "dengziji", "邓紫棋" },  { "maobuyi", "毛不易" },     { "liudehua", "刘德华" },
    { "zhangxueyou", "张学友" }, { "wangfei", "王菲" },     { "linjunjie", "林俊杰" },
    { "qingge", "情歌" },        { "gushi", "故事" },
    { "huainian", "怀念" },    { "yonggan", "勇敢" },       { "jianqiang", "坚强" },
    { "guniang", "姑娘" },     { "shaonian", "少年" },      { "qingchun", "青春" },
    { "mengxiang", "梦想" },   { "weilai", "未来" },        { "xingfu", "幸福" },
    { "kuaile", "快乐" },      { "yongyuan", "永远" },      { "fenbie", "分别" },
    { "xiangnian", "想念" },   { "xingkong", "星空" },
    { "haiyang", "海洋" },     { "tiankong", "天空" },      { "yueliang", "月亮" },
    { "taiyang", "太阳" },     { "xingxing", "星星" },      { "yuhang", "宇航" },
    { "shiguang", "时光" },    { "nianjing", "年纪" },      { "chengzhang", "成长" },
    /* 常用补字（打字高频） */
    { "rensheng", "人生" },    { "shenghuo", "生活" },      { "gongsi", "公司" },
    { "gongzuoshi", "工作室" }, { "zhengque", "正确" },     { "cuowu", "错误" },
    { "zhidao", "知道" },      { "buzhidao", "不知道" },    { "mingbai", "明白" },
    { "yinggai", "应该" },     { "keneng", "可能" },        { "xuyao", "需要" },
    { "zhichi", "支持" },      { "guanzhu", "关注" },       { "fenxiang", "分享" },
    { "biji", "笔记" },
    { "tixing", "提醒" },      { "dingwei", "定位" },       { "sousuo1", "搜索" },
    { "chaxun", "查询" },      { "xuanze", "选择" },        { "quxiao", "取消" },
    { "queren", "确认" },      { "fanhui", "返回" },        { "shezhi", "设置" },
    { "zhuti", "主题" },       { "beijing1", "背景" },      { "liangdu", "亮度" },
    { "yinliang", "音量" },    { "jingyin", "静音" },       { "chongqi", "重启" },
    { "kaiguan", "开关" },       { "xitong", "系统" },
    { "yingjian", "硬件" },    { "ruanjian", "软件" },
};

#define WORD_CNT ((int)(sizeof(s_words) / sizeof(s_words[0])))

/* 词条拼音里可能有作者为区分同音词加的尾数字（"beijing1"），跳过它们 */
static size_t py_alpha_len(const char *py)
{
    size_t n = 0;
    while (py[n] >= 'a' && py[n] <= 'z') {
        n++;
    }
    return n;
}

/* 该词对应的拼音字母数（选词后要吃掉这么多拼音） */
int ui_pinyin_word_pylen(const char *word)
{
    if (!word || !word[0]) {
        return -1;
    }
    for (int i = 0; i < WORD_CNT; i++) {
        if (strcmp(s_words[i].word, word) == 0) {
            return (int)py_alpha_len(s_words[i].py);
        }
    }
    return -1;
}

/* 一条词的拼音首字母："tianwailaiwu" → "twlw"（贪心取最长音节） */
static void word_initials(const char *py, char *out, size_t cap)
{
    size_t o = 0;
    const char *p = py;
    char t[8];
    while (*p && o + 1 < cap) {
        int best = 0;
        for (int n = 1; n <= 6 && p[n - 1]; n++) {
            if (p[n - 1] < 'a' || p[n - 1] > 'z') {
                break;
            }
            memcpy(t, p, (size_t)n);
            t[n] = 0;
            if (ui_pinyin_is_syllable(t)) {
                best = n;
            }
        }
        if (best == 0) {
            break;                  /* 切不动了，剩下的忽略 */
        }
        out[o++] = p[0];            /* 首字母 */
        p += best;
    }
    out[o] = 0;
}

static bool words_contains(const char *buf, const char *word, size_t wl)
{
    const char *p = buf;
    while (*p) {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (len == wl && strncmp(p, word, wl) == 0) {
            return true;
        }
        if (!bar) {
            break;
        }
        p = bar + 1;
    }
    return false;
}

const char *ui_pinyin_words(const char *py)
{
    static char buf[200];
    if (!py || !py[0]) {
        return NULL;
    }
    size_t n = strlen(py);
    size_t o = 0;
    buf[0] = '\0';

    /* 三轮：0=完全匹配（打全了优先出整词） 1=前缀匹配 2=首字母缩写（twlw → 天外来物） */
    for (int round = 0; round < 3 && o + 16 < sizeof(buf); round++) {
        for (int i = 0; i < WORD_CNT && o + 16 < sizeof(buf); i++) {
            size_t pl = py_alpha_len(s_words[i].py);
            bool exact = (pl == n);
            if (round == 0 && !exact) {
                continue;
            }
            if (round == 1 && exact) {
                continue;           /* 第一轮已经输出 */
            }
            if (round == 0 || round == 1) {
                if (strncmp(s_words[i].py, py, n) != 0) {
                    continue;
                }
            } else {
                /* 缩写：按首字母串前缀匹配（twlw / twl 都能出"天外来物"） */
                if (n < 2) {
                    continue;
                }
                char ini[24];
                word_initials(s_words[i].py, ini, sizeof(ini));
                if (strncmp(ini, py, n) != 0) {
                    continue;
                }
            }
            size_t wl = strlen(s_words[i].word);
            if (words_contains(buf, s_words[i].word, wl)) {
                continue;
            }
            if (o) {
                buf[o++] = '|';
            }
            memcpy(buf + o, s_words[i].word, wl);
            o += wl;
            buf[o] = '\0';
        }
    }
    return buf[0] ? buf : NULL;
}

/* ================= 输入记忆 =================
 * 用户选过的词/字记下来：下次打同样的拼音（或只打了开头）就排在最前面。
 * 存 NVS（py/learn），最多 32 条，按使用次数排序。
 */
#define LEARN_MAX 32

typedef struct {
    char py[16];        /* 上屏时消耗的拼音，如 "nihao" */
    char word[16];      /* 上屏的文字，如 "你好" */
    uint8_t used;
    uint16_t hits;
} learn_t;

static learn_t s_learn[LEARN_MAX];
static int s_learn_cnt;
static bool s_learn_loaded;

static void learn_save(void)
{
    char buf[600] = { 0 };
    size_t o = 0;
    for (int i = 0; i < s_learn_cnt && o + 44 < sizeof(buf); i++) {
        o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%s%s,%s,%d,%d",
                              i ? ";" : "", s_learn[i].py, s_learn[i].word,
                              s_learn[i].used, s_learn[i].hits);
    }
    nvs_handle_t h;
    if (nvs_open("py", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "learn", buf);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void learn_load(void)
{
    if (s_learn_loaded) {
        return;
    }
    s_learn_loaded = true;
    s_learn_cnt = 0;
    char buf[600] = { 0 };
    size_t len = sizeof(buf);
    nvs_handle_t h;
    if (nvs_open("py", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    nvs_get_str(h, "learn", buf, &len);
    nvs_close(h);
    char *save = NULL;
    for (char *t = strtok_r(buf, ";", &save); t && s_learn_cnt < LEARN_MAX;
         t = strtok_r(NULL, ";", &save)) {
        char *f[4];
        int nf = 0;
        char *p = t;
        while (nf < 4) {
            f[nf++] = p;
            char *c = strchr(p, ',');
            if (!c) {
                break;
            }
            *c = '\0';
            p = c + 1;
        }
        if (nf < 2 || !f[0][0] || !f[1][0]) {
            continue;
        }
        learn_t *e = &s_learn[s_learn_cnt++];
        strlcpy(e->py, f[0], sizeof(e->py));
        strlcpy(e->word, f[1], sizeof(e->word));
        e->used = (nf >= 3) ? (uint8_t)atoi(f[2]) : (uint8_t)strlen(e->py);
        e->hits = (nf >= 4) ? (uint16_t)atoi(f[3]) : 1;
    }
}

/* 记录一次选择。py = 这次吃掉的拼音（可能为空，比如纯符号） */
void ui_pinyin_learn(const char *py, const char *word)
{
    if (!word || !word[0]) {
        return;
    }
    learn_load();
    int pl = py ? (int)strlen(py) : 0;

    for (int i = 0; i < s_learn_cnt; i++) {
        if (strcmp(s_learn[i].word, word) == 0) {
            s_learn[i].hits = (s_learn[i].hits < 60000) ? (uint16_t)(s_learn[i].hits + 1) : 60000;
            if (pl > 0) {
                strlcpy(s_learn[i].py, py, sizeof(s_learn[i].py));
                s_learn[i].used = (uint8_t)pl;
            }
            learn_save();
            return;
        }
    }
    if (s_learn_cnt >= LEARN_MAX) {
        /* 满了就踢掉命中次数最少的 */
        int min_i = 0;
        for (int i = 1; i < s_learn_cnt; i++) {
            if (s_learn[i].hits < s_learn[min_i].hits) {
                min_i = i;
            }
        }
        for (int k = min_i; k < s_learn_cnt - 1; k++) {
            s_learn[k] = s_learn[k + 1];
        }
        s_learn_cnt--;
    }
    learn_t *e = &s_learn[s_learn_cnt++];
    memset(e, 0, sizeof(*e));
    if (pl > 0) {
        strlcpy(e->py, py, sizeof(e->py));
    }
    strlcpy(e->word, word, sizeof(e->word));
    e->used = (uint8_t)(pl > 0 ? pl : 0);
    e->hits = 1;
    learn_save();
}

/* 匹配：查询是记忆拼音的前缀（边打边出），或记忆拼音是查询的前缀（打多了） */
static bool learn_match(const char *q, const char *py)
{
    size_t ql = strlen(q), pl = strlen(py);
    if (ql == 0 || pl == 0) {
        return false;
    }
    if (ql <= pl) {
        return strncmp(py, q, ql) == 0;
    }
    return strncmp(q, py, pl) == 0;
}

/* 返回 "used|词|used|词|..."，按命中次数从高到低 */
const char *ui_pinyin_learned(const char *py)
{
    static char buf[260];
    if (!py || !py[0]) {
        return NULL;
    }
    learn_load();
    if (s_learn_cnt == 0) {
        return NULL;
    }
    int idx[LEARN_MAX];
    int n = 0;
    for (int i = 0; i < s_learn_cnt; i++) {
        if (learn_match(py, s_learn[i].py)) {
            idx[n++] = i;
        }
    }
    if (n == 0) {
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        for (int k = i + 1; k < n; k++) {
            if (s_learn[idx[k]].hits > s_learn[idx[i]].hits) {
                int t = idx[i];
                idx[i] = idx[k];
                idx[k] = t;
            }
        }
    }
    size_t o = 0;
    for (int i = 0; i < n && o + 24 < sizeof(buf); i++) {
        learn_t *e = &s_learn[idx[i]];
        o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%d|%s|", e->used, e->word);
    }
    return buf;
}
