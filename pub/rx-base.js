// ============================================================================
//  rx-base.js — основной клиентский код WebSDR (rtune / rx-websdr)
// ============================================================================
// Происхождение: websdr-base.js (PA3FWM, WebSDR.org, Copyright 2013-2018),
// переработан для проекта rx-websdr (Yaroslavl).
//
// ЛИЦЕНЗИЯ ИСХОДНИКА:
//   websdr-base.js — WebSDR HTML5 client, Copyright 2013-2018, PA3FWM@websdr.org.
//   Исходный код распространяется автором в составе оригинального сервера
//   WebSDR и разрешён к использованию в неизменном виде на серверах WebSDR.
//   Наш проект использует его как базу и вносит изменения для личного/местного
//   сервера; полные условия см. в оригинале websdr-base.js.
//
// ЧТО СДЕЛАНО В ЭТОМ ФАЙЛЕ ОТЛИЧНОГО ОТ ОРИГИНАЛА:
//   - код разбит на разделы с поясняющими комментариями (для читаемости);
//   - удалены мёртвые ветки Java-апплетов (radio-кнопки Waterfall/Sound
//     Java|HTML5, javatest, html5orjavamenu — всегда HTML5);
//   - удалены хеши для IE8 (document.write, dummyforie и пр.);
//   - дубликаты функций (setstep/setfreqb/setfreqif/timeout_idle_*) схлопнуты;
//   - загрузка водопада переведена на rx-waterfall.js (наш HTML5-клиент);
//     звук остаётся на websdr-sound.js (лицензия PA3FWM, см. шапку файла).
//
// ПРИМЕЧАНИЕ О КОНТРАКТЕ С ФРОНТЕНДОМ:
//   Файл загружается из websdr-head.html; функции, перечисленные ниже, вызываются
//   из разметки (websdr-controls.html / websdr-head.html) и НЕ должны быть
//   переименованы без правки разметки:
//   background_load, bodyonload, freq_step, imgmousedown, mem_* (см. memory),
//   mousedown*, record_click, setautonotch, setcompactview, setfreqif(_fut),
//   sethidedx, sethboost, set_magic, setmf, set_mode, setmute, setnoise,
//   settings_store, setview, set_volume, toggle_squelch, update_squelch_threshold,
//   waterfallheight, waterfallmode, wfset.
// ============================================================================

// ---------------------------------------------------------------------------
// РАЗДЕЛ 1. Состояние клиента (глобальные переменные)
// ---------------------------------------------------------------------------
// Эти переменные разделяют состояние между модулями: какой бэнд/частота/режим
// выбраны, где на экране находятся элементы управления, какие таймеры активны.
// Оставлены с именами оригинала, чтобы не менять поведение.

// "Магический глаз" (индикатор уровня сигнала, круговой прогресс)
var circle
var radius
var circumference
var divRing

// ---- что слушает пользователь ----
var lo=-2.76,hi=-0.15;   // границы полосы пропускания, кГц относительно несущей
var mode="LSB";           // режим: "LSB"/"USB"/"AM"/"FM"/"CW"
var band=0;               // id бэнда, который слушаем
var freq=freq=bandinfo[0].vfo;  // частота (несущей) в кГц
var memories = [ ];       // сохранённые частоты (в localStorage)

// ---- второй "VFO" (переключатель A/B) ----
var ab_lo=lo;
var ab_hi=hi;
var ab_mode=mode;
var ab_band=band;
var ab_freq=freq;
var ab_squelch=false;

// ---- SQL (шумоподавитель) ----
var squelch_open = false;      // текущее состояние (открыт/закрыт)
var squelch_hang_timer = null; // таймер задержки закрытия
var SQL_HYSTERESIS_DB = 1.5;   // гистерезис в дБ
var SQL_HANG_MS = 200;         // время удержания после пропадания сигнала, мс

var mem_hilite=-1;
var ab_mem_hilite=-1;

// ---- что видит пользователь ----
var Views={ allbands:0, othersslow:1, oneband:2, blind:3 };
var view=Views.blind;
var nwaterfalls=0;
var waterslowness=2;
var waterheight=100;
var watermode=1;
var scaleheight=14;

// ---- таймеры ----
var interval_updatesmeter;
var interval_ajax3;
var chseq=0;   // порядковый номер последней полученной порции статистики/чата (обновляется сервером)
var timeout_idle;
var setfreqif_fut_timer;  // таймер для ввода частоты с клавиатуры

var samplecount=0;
var windowhours=0;
var windowmins=0;
var windowsecs=0;

// информация о доступных "виртуальных" бэндах:
// содержит: effsamplerate, effcenterfreq, zoom, start, minzoom, maxzoom,
//           samplerate, centerfreq, vfo, scaleimgs, realband
var bi = new Array();
// число бэндов:
var nvbands=nbands;

// ---- переменные, управляющие настройкой ----
var geo="";
var udkflag=0;
var cw_offset=0;		// сдвиг в оценке шага настройки в CW
var tune_step=0;
var tune_old=0;
var fRND=3700;
var fRAW=3700;

// ---- ссылки на элементы экрана ----
var scaleobj;
var scaleobjs = new Array();
var scaleimgs0 = new Array();
var scaleimgs1 = new Array();
var passbandobj;
var edgelowerobj;
var edgeupperobj;
var carrierobj;
var smeterobj;
var smeterobjnew;
var numericalsmeterobj;
var smeterpeakobj;
var numericalsmeterpeakobj;
var waterfallapplet = new Array();
var soundapplet = null;

// ---- объекты для S-метра / шумовых метрик ----
var smeterminobj;	// используется в шумовых метриках
var snrobj;
var noise=0;		// шум
var snr=1;		// отношение сигнал/шум

// ---- разное ----
var serveravailable=-1;  // -1 пока не проверено, 0/1 — false/true
var smeterpeaktimer=2;
var smeterpeak=0;
var smetermintimer=2;	// таймер окна шума
var smetermin=3000;	// минимальный уровень (обновляется в окне)

var allloadeddone=false;
var waitingforwaterfalls=0;  // сколько водопадных апплетов ещё стартует
var band_fetchdxtimer=new Array();
var hidedx=0;
var isTouchDev = false;

// ---- производные величины ----
var khzperpixel=bandinfo[band].samplerate/1024;
var passbandobjstart=0;    // позиция (в пикселях) начала полосы на шкале частот
var passbandobjwidth=0;    // ширина полосы пропускания в пикселях
var centerfreq=bandinfo[band].centerfreq;
// ---------------------------------------------------------------------------
// РАЗДЕЛ 2. Инициализация страницы и вспомогательные функции
// ---------------------------------------------------------------------------
// bodyonload() вызывается из <body onload="bodyonload()"> в index.html.
// Здесь: читаются куки/настройки, строятся элементы управления (кнопки бэндов,
// переключатели режима), поднимаются водопад и звуковой апплет.

function bodyonload()
{
	// x — кука, указывающая на неверный ввод названия (geo-подстановка)
	if (x!=null)
   {
     console.log(x);
     if (x.length > 10 || /\s+/.test(document.usernameform.username.value))
     {
       ip2geo('visited');
       x="";
     }
   }
   var s;
   // HTML5-клиент всегда: Java-апплеты не поддерживаются современными
   // браузерами. Раньше здесь вызывалась html5orjavamenu(), строившая
   // радио-кнопки Waterfall/Sound Java|HTML5 — рудимент удалён.
   usejavawaterfall=false;
   usejavasound=false;

   view= readCookie('view');
   if (view==null) view=Views.oneband;

   // построить радиокнопки выбора вида (Все бэнды / Один бэнд / Выкл)
   if (nvbands>=2) s= '<input class="radios" type="radio" name="group" id="radio-1" value="all bands" onclick="setview(0);"><label for="radio-1">All Bands</label><input class="radios" type="radio" name="group" id="radio-4" value="other slow" onclick="setview(1);" style="display:none"><label for="radio-4"style="display:none"> other slow</label><input class="radios" type="radio" name="group" id="radio-2" value="one band" onclick="setview(2);"><label for="radio-2">Single Band</label>';
   else {
      s='<input class="radios" type="radio" name="group" id="radio-2" value="one band" onclick="setview(2);"><label for="radio-2">Off</label>';
      if (view==Views.othersslow || view==Views.allbands) view=Views.oneband;
   }
   s+='<input class="radios" type="radio" name="group" id="radio-3" value="blind" onclick="setview(3);"><label for="radio-3">Off</label>';
   document.getElementById('viewformbuttons').innerHTML = s;
   if (nvbands>=2) document.viewform.group[view].checked=true;
   else document.viewform.group[view-2].checked=true;

   var x= readCookie('username');

   var p=document.getElementById("please2");
   if (!x && p) p.innerHTML="<b><i>Пожалуйста, введите имя или позывной в поле <a href='#please'>вверху страницы</a>, чтобы ваши сообщения в чате были подписаны!</i></b>";

   uu_compactview=document.getElementById("compactviewcheckbox").checked;
   document.getElementById("mutecheckbox").checked=false;
   document.getElementById("gainlevelcheckbox").checked=false;
   document.getElementById("autonotchcheckbox").checked=false;

   // ---- пресеты (память частот) из localStorage ----
   try { memories=JSON.parse(localStorage.getItem('memories')); } catch (e) {};
   if (!memories) memories=[];
   else {
       // конвертация из старого формата данных (можно удалить позже)
       var rew=false;
       for (i=0;i<memories.length;i++) {
          if (memories[i].mode==1) { memories[i].mode="AM"; rew=true; }
          if (memories[i].mode==4) { memories[i].mode="FM"; rew=true; }
          if (memories[i].mode==0) {
             rew=true;
             if (memories[i].hi-memories[i].lo<1) memories[i].mode="CW";
             else if (memories[i].hi+memories[i].lo>0) memories[i].mode="USB";
             else memories[i].mode="LSB";
          }
          if (!memories[i].nomfreq) memories[i].nomfreq=memories[i].freq + (memories[i].mode=="CW"?0.75:0);
       }
       if (rew) try { localStorage.setItem('memories',JSON.stringify(memories)); } catch (e) {};
   }
   mem_show();
   passbandobj =document.getElementById('yellowbar');
   edgeupperobj = document.getElementById('edgeupper');
   edgelowerobj = document.getElementById('edgelower');
   edgeupperobj = document.getElementById('edgeupper');
   carrierobj = document.getElementById('carrier');
   smeterobj = document.getElementById('smeterbar');
   smeterobjnew = document.getElementById('smeterbarnew');
   numericalsmeterobj=document.getElementById('numericalsmeter');
   smeterpeakobj = document.getElementById('smeterpeak');
   smeterminobj = document.getElementById('smetermin');
   snrobj = document.getElementById('snr_info');
   numericalsmeterpeakobj=document.getElementById('numericalsmeterpeak');
   smeterobj.style.top= smeterpeakobj.style.top;
   smeterobj.style.left= smeterpeakobj.style.left;

   divRing = document.getElementsByClassName('progress-ring')[0]

   bi=bandinfo;
   for (i=0;i<nbands;i++) {
      var e=bi[i];
      e.realband=i;
      e.effcenterfreq=e.centerfreq;
      e.effsamplerate=e.samplerate;
      e.zoom=0;
      e.start=0;
      e.minzoom=0;
   }

   document_bandbuttons();

   document.freqform.frequency.value=freq;
   if (nbands>1) document.freqform.group0[0].checked=true;

   chatboxobj = document.getElementById('chatbox');

   statsobj = document.getElementById('stats');
   numusersobj = document.getElementById('numusers');
   usersobj = document.getElementById('users');

   setview(view);

   // если бэнд по умолчанию LSB, а мы в USB/полосе выше — переключить
   if (!islsbband(band) && hi<0) { var tmp=hi; hi=-lo; lo=-tmp; mode="USB"; }
   var tuneparam = (new RegExp("[?&]tune=([^&#]*)").exec(window.location.href));
   if (tuneparam) {
      setfreqtune(tuneparam[1]);
   } else if (ini_freq && ini_mode) {
      setfreqif(ini_freq);
      set_mode(ini_mode);
   }

   document_soundapplet();

   interval_ajax3 = setTimeout('ajaxFunction3()',1000);

   interval_updatesmeter = setInterval('updatesmeter()',100);

   if (isTouchDev) {
      registerTouchEvents("carrier", touchpassband, touchXYpassband);
      registerTouchEvents("yellowbar", touchpassband, touchXYpassband);
      registerTouchEvents("edgeupper", touchupper, touchXYupperedge);
      registerTouchEvents("edgelower", touchlower, touchXYloweredge);
   }
   settings_recall();

   // "оборудование" — раскрывающаяся панель
	$("#equip_button").click(function(){
	  $("#equip_info").slideToggle();
	});
	$(".band_offed").click(function () {
		$(".notify").toggleClass("active");
		$("#notifyType").toggleClass("success");

		setTimeout(function(){
			$(".notify").removeClass("active");
			$("#notifyType").removeClass("success");
		},3000);
		});
	// MagicEye (круговой индикатор) — инициализация
	circle = document.querySelectorAll('circle');
	radius = circle[0].r.baseVal.value;
	circumference = radius * 2 * Math.PI;

	circle[0].style.strokeDasharray = `${circumference} ${circumference}`;
	circle[0].style.strokeDashoffset = `${circumference}`;
	circle[1].style.strokeDasharray = `${circumference} ${circumference}`;
	circle[1].style.strokeDashoffset = -`${circumference}`;
}

// MagicEye — обновление кругового индикатора (0..100%)
function setProgress(percent) {
	  const offset = circumference - percent / 100 * circumference;
	  circle[0].style.strokeDashoffset = offset;
	  circle[1].style.strokeDashoffset = -offset;
	}

// вспомогательная: отмена события (stopPropagation/preventDefault)
function cancelEvent(e)
{
  e = e ? e : window.event;
  if(e.stopPropagation) e.stopPropagation();
  if(e.preventDefault) e.preventDefault();
  e.cancelBubble = true;
  e.cancel = true;
  e.returnValue = false;
  return false;
}

// ---------------------------------------------------------------------------
// РАЗДЕЛ 3. Таймаут бездействия
// ---------------------------------------------------------------------------
// Если пользователь ничего не делает idletimeout мс (задаётся сервером),
// показываем страницу "Вы неактивны" с предложением перезагрузить.

function timeout_idle_do()
{
   try { clearInterval(interval_updatesmeter); } catch (e) {} ;
   try { clearTimeout(interval_ajax3); } catch (e) {} ;
   var i;
   try { for (i=0;i<nwaterfalls;i++) waterfallapplet[i].destroy(); } catch (e) {} ;
   try { soundapplet.destroy(); } catch (e) {};

   // специальный позывной отключает таймаут ("1" или "161NS001" — отладка)
  if (document.usernameform.username.value == "1")
  {
    idletimeout=0;
  }
   if (idletimeout > 59999) {idle_sub_text = idletimeout/60000 + " min.";}
   else if (idletimeout < 60000) {idle_sub_text = idletimeout/1000 + " sec.";}

   idle_page='<div>';
     idle_page+='<div style="margin-bottom: 20px; background-color: #fff; border: 1px solid transparent; border-radius: 4px;">';
       idle_page+='<div style="padding: 25px;">';
         idle_page+='<div style="color: #ffffff; background-color: #ff0000; border-color: #ff0000; padding: 15px; border: 1px solid transparent; border-radius: 4px; text-align: center;" role="alert">';
           idle_page+='<span style="text-align: center; font-size: 24px;"><b>You are inactive.</b></span><br>';
           idle_page+='<span style="text-align: center; font-size: 18px;"><b>Time limit: '+ idle_sub_text +'</b></span>';
         idle_page+='</div>';
         idle_page+='<div style="margin-top: 25px; text-align: center;">';
           idle_page+='<button type="button" style="font-family: inherit; color: black; display: inline-block; padding: 8px; cursor: pointer; font-size: 16px;" onClick="window.location.reload()">Reload WebSDR page</button>';
         idle_page+='</div>';
       idle_page+='</div>';
     idle_page+='</div>';
   idle_page+='</div>';

   document.body.innerHTML=idle_page;
}

function timeout_idle_restart()
{
   // спец-позывной отключает таймаут (для контроля сервера/тестов)
  if (document.usernameform.username.value == "161NS001")
  {
    idletimeout=0;
  }

   if (!idletimeout) return;
   time_out = (idletimeout / 1000) / 60;
   timeout_secs = samplecount;
   try { clearTimeout(timeout_idle); } catch(e) {};
   timeout_idle=setTimeout('timeout_idle_do();',idletimeout);
}

function sessionTime()
{
  occloop = setInterval(function()
  {
    samplecount = samplecount + 1;
    windowsecs = parseInt(samplecount % 60);
    windowhours = parseInt(samplecount / 3600);
    windowmins = parseInt((samplecount % 3600) / 60);
    }, 1000);
}

// ---------------------------------------------------------------------------
// РАЗДЕЛ 4. Звуковые настройки и шумоподавитель (SQL)
// ---------------------------------------------------------------------------
// Настройки (режим, частота, полоса, имя) отправляются на сервер, который
// управляет демодулятором. SQL использует S-метр и мьют клиента.

function send_soundsettings_to_server()
{
  var m=mode;
  if (m=="USB") m=0;
  else if (m=="LSB") m=0;
  else if (m=="CW") m=0;
  else if (m=="AM") m=1;
  else if (m=="FM") m=4;
  try {
     soundapplet.setparam(
         "f="+freq
        +"&band="+band
        +"&lo="+lo
        +"&hi="+hi
        +"&mode="+m
        +"&name="+encodeURIComponent(document.usernameform.username.value)
        );
  } catch (e) {};
  timeout_idle_restart()
}

function toggle_squelch(enabled) {
    ab_squelch = enabled;
    var mask = document.getElementById("gainlevelmask");
    if (enabled) mask.classList.remove("hiddencontrol");
    else mask.classList.add("hiddencontrol");
    toggle_info('sql', enabled);
    if (!enabled && soundapplet) {
        soundapplet.setmute(false);
        squelch_open = false;
        if (squelch_hang_timer) {
            clearTimeout(squelch_hang_timer);
            squelch_hang_timer = null;
        }
    }
}
function update_squelch_threshold(val)
{
    document.getElementById('gaindb').innerHTML = "SQL " + val + " dB";
}

// автонотч — ширина полосы для подавления автогетеродина (?)
function setautonotch(a)
{
   a=Number(a);
   soundapplet.setparam("autonotch="+a);
}
function setautonotch2(a)
{
   a=Number(a);
   soundapplet.setparam2(a);
}

// DNR MOD — шумоподавление (noise reduction)
function setnoise(a)
{
   a=Number(a);
   soundapplet.setnoise(a);
}
//end DNR
function setnoisereduction(level)
// level -999 означает "выкл"
{
   a=Number(level);
   soundapplet.setparam("noisered="+a);
}
function sethboost(a)
{
   a=Number(a);
   soundapplet.sethboost(a);
}

function setmute(a)
{
   a=Number(a);
   soundapplet.setparam("mute="+a);
   try { soundapplet.setmute(a); } catch(e) {};
}

// ---------------------------------------------------------------------------
// РАЗДЕЛ 5. Полоса пропускания и S-метр
// ---------------------------------------------------------------------------
// draw_passband() рисует жёлтую полосу на шкале: где на частоте находится
// выбранный фильтр (lo..hi). S-метр обновляется из soundapplet.smeter().

function draw_passband()
{
   passbandobjstart=Math.round((lo-0.045)/khzperpixel);
   passbandobjwidth=Math.round((hi+0.045)/khzperpixel)-passbandobjstart;
   if (passbandobjwidth == 0) passbandobjwidth = 1;
   passbandobj.style.width=passbandobjwidth+"px";
   if (!scaleobj) return;

   var x=(freq-centerfreq)/khzperpixel+512;
   var maxx = parseInt(scaleobj.style.width);
   if (isTouchDev && x > maxx) x = maxx;
   var y=scaleobj.offsetTop+15;
   passbandobj.style.top=y+"px";
   edgelowerobj.style.top=y+"px";
   edgeupperobj.style.top=y+"px";
   carrierobj.style.top=y-15+"px";
   carrierobj.style.left=x+"px";
   x=x+passbandobjstart;
   passbandobj.style.left=x+"px";
   edgelowerobj.style.left=(x-11)+"px";
   edgeupperobj.style.left=(x+passbandobjwidth)+"px";
}

function volumedb(vol)
{
  document.getElementById('volumedb').innerHTML=" " + vol + "dB";
}

// ---------------------------------------------------------------------------
// РАЗДЕЛ 6. Пресеты (память частот) и переключение режимов
// ---------------------------------------------------------------------------
// Пресеты хранятся в localStorage и показываются в таблице слева.
// rememberpreset()/showhides()/showrow() управляют строками кнопок режимов
// по горизонтали (LSB/USB/AM/FM/CW).

function rememberpreset()
{
  if(mode==='LSB') {lsblo=lo; lsbhi=hi;} else
  if(mode==='USB') {usblo=lo; usbhi=hi;} else
  if(mode==='AM') {amlo=lo; amhi=hi;} else
  if(mode==='FM') {fmlo=lo; fmhi=hi} else
    {cwlo=lo; cwhi=hi;}
}

function showhides()
{
     if(mode==='LSB') {showrow('lsbpresets','usbpresets','ampresets','fmpresets','cwpresets');} else
     if(mode==='USB') {showrow('usbpresets','lsbpresets','ampresets','fmpresets','cwpresets');} else
     if(mode==='AM') {showrow('ampresets' ,'usbpresets','lsbpresets','fmpresets','cwpresets');} else
     if(mode==='FM') {showrow('fmpresets' ,'usbpresets','lsbpresets','ampresets','cwpresets');} else
       {showrow('cwpresets' ,'usbpresets','lsbpresets','ampresets','fmpresets');}
}

function showrow(visiblerow,h1,h2,h3,h4)
{
  var visiblerow;
  var h1;
  var h2;
  var h3;
  var h4;

  {document.getElementById(visiblerow).style.display = "table-row";}
  {document.getElementById(h1).style.display = "none";}
  {document.getElementById(h2).style.display = "none";}
  {document.getElementById(h3).style.display = "none";}
  {document.getElementById(h4).style.display = "none";}
}

// ---------------------------------------------------------------------------
// РАЗДЕЛ 7. Сохранение/восстановление настроек (localStorage)
// ---------------------------------------------------------------------------
// settings_store() пишет настройки интерфейса (громкость, вид, полоса, SQL),
// settings_recall() восстанавливает их при следующем заходе.

function settings_store()
{
   var s={};
   s.allowkeys=document.viewform.allowkeys.checked;
   s.compactview=document.getElementById('compactviewcheckbox').checked;
   s.volume=document.getElementById('volumecontrol2').value;

   s.band=band;
   s.freq=Math.round(freq * 100) / 100;
   s.mode=mode;
   s.lo=lo;
   s.hi=hi;
   s.hidedx=hidedx;
   s.squelch_enabled = document.getElementById('gainlevelcheckbox').checked;
   s.squelch_threshold = document.getElementById('manualgain').value;
   s.waterfallheight=waterheight;
   s.background=document.getElementById('background_toggle').checked
   s.divRingOpacity=divRing.style.opacity
   try { localStorage.setItem('settings',JSON.stringify(s)); } catch (e) {};
}

function settings_recall()
{
   var s;
   try { s=JSON.parse(localStorage.getItem('settings')); } catch (e) {  return; };
   if (!s) {
	   document.getElementById('background_toggle').checked = true;
	   background_load();
	   return;
	  }
   document.viewform.allowkeys.checked=s.allowkeys;
   document.getElementById('compactviewcheckbox').checked=s.compactview;
   if (s.divRingOpacity) {
	   divRing.style.opacity=s.divRingOpacity;
	   document.getElementById('magicinput').value=s.divRingOpacity;
   }
   if (s.volume) document.getElementById("volumecontrol2").value=s.volume;
   if (s.volume) volumedb(s.volume);
	if (s.hidedx) {
		sethidedx(s.hidedx);
		document.getElementById('hidedx').checked=s.hidedx;
	} else  {
		sethidedx(s.hidedx);
	document.getElementById('hidedx').checked=s.hidedx;
	}
   //if (s.band) {band=s.band; setband(band);}
   //if (s.freq) freq=s.freq;
   //if (s.mode) {mode=s.mode; set_mode(mode);}
   //if (s.lo) lo=s.lo;
   //if (s.hi) hi=s.hi;
   //if (s.gain) document.getElementById("manualgain").value=s.gain;
   //if (s.gain) gaindb(s.gain);
   //if (s.waterfallheight) waterheight=s.waterfallheight;
   if (s.background) {
	   document.getElementById('background_toggle').checked = true;
	   background_load();
   }
   var c=document.getElementsByName('wf-size');
   if (s.squelch_enabled !== undefined) {
       document.getElementById('gainlevelcheckbox').checked = s.squelch_enabled;
       toggle_squelch(s.squelch_enabled);
   }
   if (s.squelch_threshold) {
       document.getElementById('manualgain').value = s.squelch_threshold;
       update_squelch_threshold(s.squelch_threshold);
   }
   var i;
   for (i=0;i<c.length;i++)
      if (c[i].value-waterheight>=0) {
         c[i].checked=true;
         break;
      }
}

// громкость (в дБ) → линейный множитель для звукового апплета
function set_volume(v)
{
    try { soundapplet.setvolume(Math.pow(10,v/10.)) } catch (e) {};
    settings_store();
}

// MagicEye — прозрачность кругового индикатора
function set_magic(o)
{
    divRing.style.opacity=o
    settings_store();
}

// ---------------------------------------------------------------------------
// РАЗДЕЛ 8. Вычисление частоты, видимости и DX
// ---------------------------------------------------------------------------
// Эти функции переводят частоты в пиксели на шкале и обратно. DX-кластер
// (dxs) — список активных станций, показывается над шкалой.

function iscw()
{
   return hi-lo < 1.0;
}

function nominalfreq()
{
   if (iscw()) return freq+(hi+lo)/2;
   return freq;
}

function freq2x(f,b)
{
   return (f-bi[b].effcenterfreq)*1024/bi[b].effsamplerate+512;
}

function wf_freq_visible(b,f)
{
   if (waitingforwaterfalls>0) return;
   var x = freq2x(f,b);
   return(x>=0 && x<1024);
}

function setwaterfall(b,f)
{
   if (waitingforwaterfalls>0) return;
   var x = freq2x(f,b);
   if (x<0 || x>=1024) wfset_freq(b, bi[b].zoom, f);
}

function dx(freq,mode,text)
{
   dxs.push( { freq:freq, mode:mode, text:text } );
}

function setfreqm(b,f,mo)
{
   setband(b);
   set_mode(mo);
   if (iscw()) f-=(hi+lo)/2;
   setfreq(f);
}

function showdx(b)
{
   var s='';
   if (!hidedx) {
      var mems=memories.slice();
      for (i=0;i<mems.length;i++) mems[i].nr=i;
      mems.sort(function(a,b){return a.nomfreq-b.nomfreq});
      for (i=0;i<dxs.length;i++) {
         var x = freq2x(dxs[i].freq,b);
         var nextx;
         if (x>1024) break;
         if (i<dxs.length-1) nextx=freq2x(dxs[i+1].freq,b);
         else nextx=1024;
         if (nextx>=1024) nextx=1280;
         if (x<0) continue;
         var fr=dxs[i].freq;
         var mo=dxs[i].mode;
         s+='<div title="" class="statinfo2" style="max-width:'+(nextx-x)+'px;left:'+(x-6)+'px;top:'+(44-scaleheight)+'px;">';
         s+='<div class="statinfo1"><div class="statinfo0" onclick="setfreqm(b,'+fr+','+"'"+mo+"'"+');">'+dxs[i].text+'<\/div><\/div><\/div>';
         s+='<div title="" class="statinfol" style="width:1px;height:44px;position:absolute;left:'+x+'px;top:-'+scaleheight+'px;"><\/div>';
      }
      for (i=0;i<mems.length;i++) if (mems[i].band==b) {
         var x=freq2x(mems[i].nomfreq,b);
         var nextx;
         if (x>1024) break;
         if (i<mems.length-1) nextx=freq2x(mems[i+1].nomfreq,b);
         else nextx=1024;
         if (nextx>=1024) nextx=1280;
         if (x<0) continue;
         var fr=mems[i].freq;
         var mo=mems[i].mode;
         s+='<div title="" class="statinfo2l" style="max-width:'+(nextx-x)+'px;left:'+(x-6)+'px;top:'+(64-scaleheight)+'px;">';
         var l=mems[i].label;
         if (!l || l=='') l='mem '+mems[i].nr;
         s+='<div class="statinfo1l"><div class="statinfo0l" onclick="setfreqm(b,'+fr+','+"'"+mo+"'"+');">'+l+'<\/div><\/div><\/div>';
         s+='<div title="" class="statinfoll" style="width:1px;height:64px;position:absolute;left:'+x+'px;top:-'+scaleheight+'px;"><\/div>';
      }
   }
   // Маркеры станций из stationinfo.txt
   if (!hidedx && bi[b].stations) {
      for (var si=0; si<bi[b].stations.length; si++) {
         var st = bi[b].stations[si];
         var x = freq2x(st.freq, b);
         if (x<0 || x>=1024) continue;
         s += '<div style="position:absolute;left:'+x+'px;top:0px;width:1px;height:24px;background-color:#00ff00;"></div>';
         s += '<div style="position:absolute;left:'+(x-20)+'px;top:24px;width:auto;max-width:80px;height:16px;border:1px solid #888;background:#222;color:#0f0;font-size:10px;text-align:center;line-height:16px;cursor:pointer;white-space:nowrap;overflow:hidden;padding:0 4px;border-radius:2px;" onclick="setfreqm('+b+','+st.freq+',\''+st.mode+'\');">'+st.name+'</div>';
      }
   }
   document.getElementById('blackbar'+band2id(b)).innerHTML=s;
   if (s!='') {
      document.getElementById('blackbar'+band2id(b)).style.height='64px';
   } else {
      document.getElementById('blackbar'+band2id(b)).style.height='30px';
   }
   draw_passband();
}

// Наш сервер не реализует /~~fetchdx и отвечает 404. Чтобы не дёргать его
// повторно на каждом переключении бэнда, после первого 404 ставим флаг.
// Метки станций из bi[b].stations (stationinfo) рисует showdx() независимо.
var dxserverdead=false;

function fetchdx(b)
{
  var xmlHttp;
  if (dxserverdead) { showdx(b); return; }
  try { xmlHttp=new XMLHttpRequest(); }
    catch (e) { try { xmlHttp=new ActiveXObject("Msxml2.XMLHTTP"); }
      catch (e) { try { xmlHttp=new ActiveXObject("Microsoft.XMLHTTP"); }
        catch (e) { alert("Your browser does not support AJAX!"); return false; } } }
  xmlHttp.onreadystatechange=function()
    {
    if(xmlHttp.readyState==4)
      {
        if (xmlHttp.status==404) { dxserverdead=true; showdx(b); return; }
        if (xmlHttp.responseText!=""||memories.slice()) {
          // fetchdx отдаёт JS-код с вызовами dx(); если сервер отвечает 404
          // (HTML), eval упадёт с SyntaxError — игнорируем такой ответ. Метки
          // станций из bi[b].stations (stationinfo) всё равно рисует showdx().
          if (xmlHttp.responseText.charAt(0) != '<') eval(xmlHttp.responseText);
          showdx(b);
        }
      }
    }
  var url="/~~fetchdx?min="+(bi[b].effcenterfreq-bi[b].effsamplerate/2)+"&max="+(bi[b].effcenterfreq+bi[b].effsamplerate/2);
  xmlHttp.open("GET",url,true);
  xmlHttp.send(null);
}

// переключение изображений шкалы при зуме/пане
function setscaleimgs(b,id)
{
   var e=bi[b];
   var st=e.start>>(e.maxzoom-e.zoom);
   if (st<0) scaleimgs0[id].src="scaleblack.png";
   else scaleimgs0[id].src = e.scaleimgs[e.zoom][st>>10];
   if (e.scaleimgs[e.zoom][1+(st>>10)]) scaleimgs1[id].src = e.scaleimgs[e.zoom][1+(st>>10)];
   else scaleimgs1[id].src="scaleblack.png";
   st+=1024;
   scaleimgs0[id].style.left = (-(st%1024))+"px";
   scaleimgs1[id].style.left = (1024-(st%1024))+"px";
}

// ---------------------------------------------------------------------------
// РАЗДЕЛ 9. Зум водопада и шаг настройки
// ---------------------------------------------------------------------------
// При зуме/пане клиент пересчитывает ширину одного пикселя (khzperpixel) и
// запоминает позицию (start) в кэше картинок шкалы.

function zoomchange(id,zoom,start)
{
   var b=id2band(id);
   var e=bi[b];
   var oldzoom=e.zoom;
   e.effsamplerate = e.samplerate/(1<<zoom);
   e.effcenterfreq = e.centerfreq - e.samplerate/2 + (start*(e.samplerate/(1<<e.maxzoom))/1024) + e.effsamplerate/2;
   e.zoom=zoom;
   e.start=start;
   setscaleimgs(b,id);
   if (b==band) {
      khzperpixel = bi[band].effsamplerate/1024;
      centerfreq = bi[band].effcenterfreq;
      updbw();
   }
   if (!hidedx) {
      clearTimeout(band_fetchdxtimer[b]);
      if (zoom!=oldzoom) {
         dxs=[]; document.getElementById('blackbar'+id).innerHTML="";
         fetchdx(b);
      } else {
         {
            showdx(b);
            band_fetchdxtimer[b] = setTimeout('dxs=[]; fetchdx('+b+');',400);
         }
      }
   }
}

// показывать/не показывать текстовую частоту (используется для типового ввода)
var dont_update_textual_frequency=false;

// шаг настройки из радиокнопок "step" (0..5)
function setstep()
{
  if (document.getElementsByName("step")[5].checked) {tune_step=5} else  //10kHz
  if (document.getElementsByName("step")[4].checked) {tune_step=4} else  // 1kHz
  if (document.getElementsByName("step")[3].checked) {tune_step=3} else  // 500Hz
  if (document.getElementsByName("step")[2].checked) {tune_step=2} else  // 100Hz
  if (document.getElementsByName("step")[1].checked) {tune_step=1} else  //  50Hz
  if (document.getElementsByName("step")[0].checked) {tune_step=0};      //  off
}

// установить частоту и обновить интерфейс
function setfreq(f)
{
   try { clearTimeout(setfreqif_fut_timer); } catch (e) {} ;
   freq=f;
   send_soundsettings_to_server();
   if (view!=Views.blind) draw_passband();
   if (dont_update_textual_frequency) return;
   var nomfreq=nominalfreq();
   if (freq.toFixed) document.freqform.frequency.value=nomfreq.toFixed(2);
   else document.freqform.frequency.value=nomfreq+" kHz";
}

// установить частоту, при необходимости переключив бэнд
function setfreqb(f)
// sets frequency but also autoselects band
{
   if (iscw()) f-=(hi+lo)/2;
   var e=bi[band];
   if (f>e.centerfreq-e.samplerate/2-4 && f<e.centerfreq+e.samplerate/2+4) {
      // новая частота в текущем бэнде
      setwaterfall(band,f);
      setfreq(f);
      return;
   }
   // новая частота вне текущего бэнда: ищем подходящий бэнд
   for (i=0;i<nvbands;i++) {
      e=bi[i];
      c=e.centerfreq;
      w=e.samplerate/2+4;
      if (f>c-w && f<c+w) {
         e.vfo=f;
         setband(i);
         return;
      }
   }
}

// ввод частоты с клавиатуры (из текстового поля)
function setfreqif(str)
{
	str= str.toString()
   f=parseFloat(str);
   if (!(f>0)) return;
   dont_update_textual_frequency=true;
   setfreqb(f);
   dont_update_textual_frequency=false;
   if (str.includes('.')) {
	   document.freqform.frequency.value=str;
   } else{
	   document.freqform.frequency.value=str+'.00';
   }
   document.freqform.frequency.blur();
}

// ввод частоты «на лету»: откладываем установку на 2с, пока печатают
function setfreqif_fut(str)
{
   try { clearTimeout(setfreqif_fut_timer); } catch (e) {} ;
   setfreqif_fut_timer = setTimeout('setfreqif('+str+')',2000);
}

// выделить кнопку выбранного режима
function pushButton(mode, lo, hi)
{ mode = mode.toLowerCase()
  try {
    document.querySelectorAll('.btnBandW').forEach(function (e) {e.classList.remove('btn-selected');})
    command = "setmf('"+mode+"', "+lo+', '+hi+");  rememberpreset();";
    document.querySelector(`[onclick="${command}"]`).classList.add('btn-selected')
  } catch(e) {};
}

// установить режим/полосу напрямую (вызывается из кнопок)
function setmf(m, l, h)
{
   mode=m.toUpperCase();
   lo=l;
   hi=h;
   updbw();
}

function set_mode(m)
{
   switch (m.toUpperCase()) {
      case "USB": setmf("usb", 0.15,  2.76); showhides(); break;
      case "LSB": setmf("lsb", -2.76, -0.15); showhides(); break;
      case "AM":  setmf("am", -4.96,  4.95); showhides(); break;
      case "CW":  setmf("cw", -0.95,  -0.55); showhides(); break;
      case "FM":  setmf("fm", -6.2,  6.21); showhides(); break;
   }
}

// шаг настройки при +/- / частота-точка
function freqstep(st)
{
   var f=nominalfreq();
   var wfvis=wf_freq_visible(band,f);

   if (st == "9") {
      if(mode=="CW") {
         f = Math.round(f);
         setfreq(f-(hi+lo)/2);
      }
      else  {
         f = Math.round(f);
         setfreq(f);
      }
   }
   else {

   var minstep=bandinfo[band].tuningstep;
   var steps_ssb= [0.02, 0.5, 2.5 ];
   var steps_am5= [0.1, 1, 5];
   var steps_am9= [0.1, 1, 9];
   var steps_am10= [0.1, 1, 10];
   var steps_fm= [1, 5, 12.5 ];
   var steps=steps_ssb;
   var grid=true;
   var i=Math.abs(st)-1;
   if (mode=="AM" || mode=="AMSYNC" || mode=="AMCOSTAS") {
      if (freq>800) steps=steps_am5;
      else {
         if (mw9kHzsteps) steps=steps_am9;
         else steps=steps_am10;
      }
   }
   if (mode=="FM") {
      steps=steps_fm;
   }
   var d=steps[i];
   var f=(st>0)?f:-f;
   if (!grid) f=f+d;
   else {
       var f0=f;
      f=d*Math.ceil(f/d+0.1);
      if (steps==steps_am9)
         if (f==180) f=183;
         else if (f==-180) if (f0<-183) f=-183; else f=-171;
   }
   f=(st>0)?f:-f;
   if (iscw()) f-=(hi+lo)/2;
   setfreq(f);
   if (wfvis) setwaterfall(band,f);
   }
}

// установка частоты из URL (?tune=)
function setfreqtune(s)
{
   var param = new RegExp("([0-9.]*)([^&#]*)").exec(s);
   if (!param[1]) return;
   if (param[2]) set_mode(param[2]);
   setfreqif(param[1]);
}

// шаги настройки: -1 / 9 / +1 (кнопки < > и выравнивание)
function freq_step(i)
{
  if (i=="-1") {udkflag=1; tune_old=tune_step; tune_step=0; freqstep(-1); tune_step=tune_old; setstep();}
  if (i=="9") {tune_step=0; freqstep(9); setstep();}
  if (i=="+1") {udkflag=1; tune_old=tune_step; tune_step=0; freqstep(+1); tune_step=tune_old; setstep();}
}

// ---------------------------------------------------------------------------
// РАЗДЕЛ 12. Память частот
// ---------------------------------------------------------------------------
function mem_recall(i)
{
   setband(memories[i].band);
   mode=memories[i].mode;
   lo=memories[i].lo;
   hi=memories[i].hi;
   showhides();
   updbw();
   setfreq(memories[i].freq);
   setwaterfall(band,memories[i].freq);
   toggle_info('mode', mode=mode);
}

function mem_erase(i)
{
   var b=memories[i].band;
   memories.splice(i,1);
   mem_show();
   showdx(b);
   try { localStorage.setItem('memories',JSON.stringify(memories)); } catch (e) {};
}

function mem_store(i)
{
   var nomf=nominalfreq();
   var l;
   try { l=memories[i].label;} catch(e){ l=''; };
   memories[i]={freq:freq, nomfreq:nomf, band:band, mode:mode, lo:lo, hi:hi, label:l };
   mem_show();
   showdx(memories[i].band);
   try { localStorage.setItem('memories',JSON.stringify(memories)); } catch (e) {};
}

function mem_label(i,nw)
{
   memories[i].label=nw;
   showdx(memories[i].band);
   try { localStorage.setItem('memories',JSON.stringify(memories)); } catch (e) {};
}

function mem_show()
{
   var i;
   var s="";
   for (i=0;i<memories.length;i++) {
      var m="";
      m=memories[i].mode;
      s+='<tr>';
     // removed class="btnNA" from each line in memory ( <input type="button" class="btnNA" title=")
      s+='<td><input type="button" class="btnMem" title="Update saved frequency" value="Update" style="border-radius: 40px 0px 0px 40px; vertical-align:text-bottom; width:100%;" onclick="mem_store('+i+')"></td>';
      s+='<td><input type="button" class="btnMem" title="Delete current memory" value="Erase" style="border-radius: 0px 40px 40px 0px; vertical-align:text-bottom; width:100%;" onclick="mem_erase('+i+')"></td>';
      s+='<td><center><input type="button" class="btn" style="width: auto; font-weight: bold; font-size: 11px;" title="Listen this frequency" value="'+memories[i].nomfreq.toFixed(2)+'&#13;&#10;KHz '+m+'" onclick="mem_recall('+i+')"></center></td>'; s+='<td><input placeholder="mem '+i+'" title="Label for this memory location" type="text" size=4 onchange="mem_label('+i+',this.value)" value="'+memories[i].label+'"></td>';

      if (memories.length<=2) s+='<td> </td><td> </td>';
      else {
        if (i<memories.length-1) {
          s+='<td><input type="button" class="btn" title="move down" style="font-size: 10px;" value="&#9660;" onclick="mem_down('+i+')"></td>';
        }
        else s+='<td><input type="button" class="btn" title="move down" style="font-size: 10px;" value="&#9660;" onclick="mem_down('+i+')" disabled></td>';
        if (i>0) s+='<td><input type="button" class="btn" title="move up" style="font-size: 10px;" value="&#9650;" onclick="mem_up('+i+')"></td>';
    else s+='<td><input type="button" class="btn" title="move up" style="font-size: 10px;" value="&#9650;" onclick="mem_up('+i+')" disabled></td>';
      }
      s+='</tr>';
   }
   s+='<tr>';
   s+='<td></td>';
   s+='<td></td>';
   s+='<td><center><input type="button" class="btnMem" title="Save current frequency to memory" value="SAVE" onclick="mem_store('+i+')"></center></td>';
   s+='</tr>';
   document.getElementById('memories').innerHTML='<table>'+s+'</table>';
}

// переключение A/B VFO
function vfos_toggle()
{
   var tmp;
   tmp=ab_lo; ab_lo=lo; lo=tmp;
   tmp=ab_hi; ab_hi=hi; hi=tmp;
   tmp=ab_mode; ab_mode=mode; mode=tmp;
   tmp=ab_band; ab_band=band; band=tmp;
   tmp=ab_freq; ab_freq=freq; freq=tmp;
   tmp=ab_squelch; ab_squelch=document.getElementById('gainlevelcheckbox').checked; document.getElementById("gainlevelcheckbox").checked=tmp;
   toggle_squelch(tmp);
   tmp=ab_mem_hilite; ab_mem_hilite=mem_hilite;
   setband(band);
   setfreq(freq);
   updbw();
   mem_hilite=tmp;
   if (mem_hilite>=0) document.getElementById('membutton'+mem_hilite).style.backgroundColor='#ffff80';
   setwaterfall(band,freq);
   showhides();
}

function vfos_equal()
{
   ab_lo=lo;
   ab_hi=hi;
   ab_mode=mode;
   ab_band=band;
   ab_freq=freq;
   ab_mem_hilite=mem_hilite;
}

// ---------------------------------------------------------------------------
// РАЗДЕЛ 13. Команды зума водопада
// ---------------------------------------------------------------------------
// wfset(cmd): 0=zoom out, 1=zoom in, 2=zoom to max (частота), 3=±100 кГц,
// 4=zoom 0 (весь бэнд).

function wfset_freq(b, zoom, f)
{
   var id=band2id(b);
   var e=bi[b];
   var effsamplerate = e.samplerate/(1<<zoom);
   var start = ( f - e.centerfreq + e.samplerate/2 - effsamplerate/2 )*1024/(e.samplerate/(1<<e.maxzoom));
   waterfallapplet[id].setzoom(zoom, start);
   timeout_idle_restart()
}

function wfset(cmd)
{
   var b=band;
   var e=bi[b];
   var id=band2id(b);
   timeout_idle_restart()
   if (cmd==0) {
      var x=512;
      waterfallapplet[id].setzoom(-2, x);
      return;
   }
   if (cmd==1) {
      var x=512;
      waterfallapplet[id].setzoom(-1, x);
      return;
   }
   if (cmd==2) {
      wfset_freq(b, e.maxzoom, freq);
   }
   if (cmd==3) {
      var min,max;
      min=freq-100; max=freq+100
      var center = (max+min)/2;
      var width = max-min;
      var j=0;
      while (2*width<e.samplerate && j<e.maxzoom) { j++; width=width*2; }
      wfset_freq(b, j, center);
      wfset_freq(b+10, j, center);
   }
   if (cmd==4) {
      waterfallapplet[id].setzoom(0, 0);
   }
}

// ---------------------------------------------------------------------------
// РАЗДЕЛ 14. Виды (все бэнды / один бэнд / выкл)
// ---------------------------------------------------------------------------
function setview(v)
{
   timeout_idle_restart()
   if ((v==Views.allbands && view==Views.othersslow) || (view==Views.allbands && v==Views.othersslow)) {
      // нет нужды перезапускать апплеты в этом случае
      view=v;
      createCookie("view",view,3652);
      waterfallspeed(waterslowness);
      return;
   }

   if (view==Views.blind) {
      var els = document.getElementsByTagName('*');
      for (i=0; i<els.length; i++) {
         if (els[i].className=="hideblind") els[i].style.display="inline";
         if (els[i].className=="showblind") els[i].style.display="none";
      }
   }
   for (i=0;i<nwaterfalls;i++) { try { waterfallapplet[i].destroy(); } catch (e) {}; }

   view=v;
   createCookie("view",view,3652);

   // Устанавливаем высоту ДО создания канвасов, чтобы они создавались
   // правильного размера для нового вида (waterfallheight() не работает пока ждём)
   if (v==Views.oneband) waterheight=250;
   else if (v==Views.allbands) waterheight=75;

   document_waterfalls();  // (пере)запуск водопадных апплетов

   var wfs=document.getElementById('wf-size');
   if (wfs) wfs.value=waterheight;

   if (view==Views.blind) {
      var els = document.getElementsByTagName('*');
      for (i=0; i<els.length; i++) {
         if (els[i].className=="showblind") els[i].style.display="inline";
         if (els[i].className=="hideblind") els[i].style.display="none";
      }
      return;
   }
}

function islsbband(b)
{
   // true если бэнд по умолчанию использует LSB
   var e=bi[b];
   if (e.centerfreq>3500 && e.centerfreq<4000) return 1;
   if (e.centerfreq>1800 && e.centerfreq<2000) return 1;
   if (e.centerfreq>7000 && e.centerfreq<7400) return 1;
   return 0;
}

// ---------------------------------------------------------------------------
// РАЗДЕЛ 15. Выбор бэнда
// ---------------------------------------------------------------------------
function setband(b)
{
   if (b<0 || b>=nvbands) return;
   bi[band].vfo=freq;

   if (islsbband(band)!=islsbband(b)) {
	   // если нужно, меняем LSB/USB местами
      var tmp=hi;
      hi=-lo;
      lo=-tmp;
      if (mode=="USB") {
		  mode="LSB";
		  set_mode('lsb');
	  } else if (mode=="LSB") {
		  mode="USB";
		  set_mode('usb')
	  }
   }

   band=b;
   var e=bi[b];
   if (nbands>1) document.freqform.group0[band].checked=true;
   if (view==Views.allbands || view==Views.othersslow) {
      scaleobj = scaleobjs[b];
   } else if (view==Views.oneband) {
      scaleobj = scaleobjs[0];
      setscaleimgs(b,0);
      if (waitingforwaterfalls==0) waterfallapplet[0].setband(b, e.maxzoom, e.zoom, e.start);
      if (!hidedx) {
         clearTimeout(band_fetchdxtimer[b]);
         dxs=[]; document.getElementById('blackbar0').innerHTML="";
         fetchdx(b);
       }
   }
   setwaterfall(b,e.vfo);
   centerfreq = e.effcenterfreq;
   khzperpixel = e.effsamplerate/1024;
   setfreq(e.vfo);
   waterfallspeed(waterslowness);

   try {
      var bb=document.getElementById('bandbuttons');
      if (bb) {
         for (var i=0;i<bb.children.length;i++)
            bb.children[i].classList.remove('btn-selected');
         var btn=document.getElementById('btnB-'+band);
         if (btn) btn.classList.add('btn-selected');
      } else {
         document.getElementById('btnB-'+band).classList.add('btn-selected');
         var ar=['0','1','2','3','4','5','6','7','8','9'];
         for (var i=0;i<ar.length;i++) if (ar[i]!=band) document.getElementById('btnB-'+ar[i]).classList.remove('btn-selected');
      }
   } catch(e) {};
   setTimeout(' smetermintimer=0',1000)

   if (!hidedx) showdx(band);
}

// показать/скрыть метки DX на шкале
function sethidedx(h)
{
   hidedx=h;
   if (view==Views.oneband) {
      if (hidedx) {
         dxs=[]; document.getElementById('blackbar0').innerHTML="";
         clearTimeout(band_fetchdxtimer[band]);
         document.getElementById('blackbar0').style.height='30px';
      } else {
         showdx(band);
         fetchdx(band);
      }
   } else {
      for (b=0;b<nvbands;b++) {
         if (hidedx) {
            dxs=[]; document.getElementById('blackbar'+band2id(b)).innerHTML="";
            clearTimeout(band_fetchdxtimer[b]);
            document.getElementById('blackbar'+band2id(b)).style.height='30px';
            draw_passband();
         } else {
            showdx(b);
            fetchdx(b);
         }
      }
   }
}

// ---------------------------------------------------------------------------
// РАЗДЕЛ 16. S-метр, шум и SNR
// ---------------------------------------------------------------------------
// updatesmeter() вызывается каждые 100 мс; читает smeter() из звукового
// апплета, рисует полосы (peak/min), считает SNR и перерисовывает график.

var sgraph={
   prevt: 0,
   e0: 80,	// текущая верхняя граница шкалы
   e1: -190,	// текущая нижняя граница шкалы
   d0: 80,	// текущая оценка минимального значения
   d1: -190,	// текущая оценка максимального значения
   width: 200,
   cnt: 0
};

function s2y(s)
{
   return sgraph.cv.height-(s-sgraph.e0)/(sgraph.e1-sgraph.e0)*sgraph.cv.height;
}

function round(value, step) {
    step || (step = 1.0);
    var inv = 1.0 / step;
    return Math.round(value * inv) / inv;
}

function updatesmeter()
{
   if (!allloadeddone) return;

   try {
      var s=soundapplet.smeter();
   } catch (e) { s=0; };
   if (s>=0) {
	   block_width = document.getElementsByClassName('smetertable')[0].rows[0].cells[0].getBoundingClientRect().width
	   smeterobj.style.width= s*0.0191667*1.08+"px";
	   blocks = Math.round(s*0.0191667*1.08/block_width);
	   smeterobjnew.style.width= block_width*blocks +"px";

   }
   else smeterobj.style.width="0px";
   smeterpeaktimer--;
   if ((smeterpeak<s-0.1) || (smeterpeaktimer<=0)) {
      smeterpeak=s;
      smeterpeaktimer=10;

      if (smeterpeak >= 0) {
            new_width = smeterpeak * 0.0191667 *1.08
            if (parseFloat(smeterpeakobj.style.width)-new_width > 0) smeterpeakobj.style.transition = '0.3s width'
            else smeterpeakobj.style.transition = '0.1s width'
            smeterpeakobj.style.width = new_width + "px";
            }
      else smeterpeakobj.style.width="0px";
      var c=''+(s/100.0-127).toFixed(1); sig=Number(c);
   }

	smetermintimer--;
	if ((smetermin>s-100) || (smetermintimer<=0))
		{
		smetermin=s;
		if (s==0) smetermintimer=2;else
		{	if (mode=="CW")	{smetermintimer=20;}else
		  if (mode=="AM")	{smetermintimer=200;}else
				  if (mode=="FM") {smetermintimer=200;}else
			  {smetermintimer=600;}
		}

		if (smetermin>=0) {
          new_width = smetermin * 0.0191667
          if (parseFloat(smeterminobj.style.width)-new_width < 0) smeterminobj.style.transition = '10s width'
          else smeterminobj.style.transition = '0.1s width'
          smeterminobj.style.width = new_width + "px";
          smeterminobj.style.width= (smetermin*0.0191667)*1.08 +"px";
		}
		else smeterminobj.style.width="0px";
		}
	snrValue=Math.round((smeterpeak-smetermin)/100)
	snrobj.textContent = snrValue
	// MagicEye
	if (snrValue<45) {
		setProgress(snrValue/0.8);
	} else {		setProgress(45/0.8)}

   if (serveravailable<0) test_serverbusy();

// Далее графический вывод уровня сигнала

   var sgraphchoiceobj=document.getElementById('sgraphchoice');
   var v=sgraphchoiceobj?sgraphchoiceobj.value:0;		// PA0SIM v — выбранное пользователем число
   if (!(v>0)) {		// PA0SIM нет графика если v==0
      if (sgraph.cv) {
         sgraph.ct.clearRect(0,0,sgraph.cv.width, sgraph.cv.height);
         sgraph.cv.style.display='none';
         sgraph.cv=null;
         sgraph.e0=80;
         sgraph.e1=-190;
      }
      return;
   }

   if (!sgraph.cv) {
      sgraph.cv=document.getElementById('sgraph');
      sgraph.cv.style.display='';
      sgraph.ct=sgraph.cv.getContext("2d");
   }
   var cv=sgraph.cv;
   var ct=sgraph.ct;
   sgraph.width=cv.width-50;

   s=s/100.0-127;
   // оценить полезный диапазон значений, не храня все точки, и перерисовать ось при необходимости
   if (sgraph.d0>s) sgraph.d0=s; else sgraph.d0+=0.1/v;
   if (sgraph.d1<s) sgraph.d1=s; else sgraph.d1-=0.1/v;
   var redrawaxis=0;
   if (sgraph.d0>sgraph.e0+15 || sgraph.d0<sgraph.e0) {
      var e0=10*Math.floor(sgraph.d0/10)-5;
      if (e0>sgraph.e0) ct.drawImage(cv, 0,0, sgraph.width,cv.height*(sgraph.e1-e0)/(sgraph.e1-sgraph.e0), 0,0,sgraph.width,cv.height);
      else {
         var f=(sgraph.e1-sgraph.e0)/(sgraph.e1-e0);
         ct.drawImage(cv, 0,0, sgraph.width,cv.height, 0,0,sgraph.width,cv.height*f);
         ct.fillStyle="white";
         ct.fillRect(0,Math.floor(cv.height*f),sgraph.width,cv.height*(1-f)+1);
      }
      sgraph.e0=e0;
      redrawaxis=1;
   }
   if (sgraph.d1>sgraph.e1 || sgraph.d1<sgraph.e1-15) {
      var e1=10*Math.ceil(sgraph.d1/10)+5;
      if (e1<sgraph.e1) {
         var f=(e1-sgraph.e0)/(sgraph.e1-sgraph.e0);
         if (f<0) f=0;
         ct.drawImage(cv, 0,cv.height*(1-f), sgraph.width,cv.height*f, 0,0,sgraph.width,cv.height);
      } else {
         var f=(sgraph.e1-sgraph.e0)/(e1-sgraph.e0);
         if (f<0) f=0;
         ct.drawImage(cv, 0,0, sgraph.width,cv.height, 0,cv.height*(1-f),sgraph.width,cv.height*f);
         ct.fillStyle="white";
         ct.fillRect(0,0,sgraph.width,Math.ceil(cv.height*(1-f)));
      }
      sgraph.e1=e1;
      redrawaxis=1;
   }
   if (redrawaxis) {
      ct.clearRect(sgraph.width,0,cv.width-sgraph.width,cv.height);
      var w=sgraph.e0;
      ct.fillStyle="black";
      ct.font="10px Verdana";
      while ((w=10*Math.ceil(w/10))<=sgraph.e1) {
         var y=s2y(w);
         ct.fillText(w+" dB",sgraph.width+2,y+4,cv.width-sgraph.width);
         w+=1;
      }
   }

   sgraph.cnt++;
   if (sgraph.cnt>=v) {				// PA0SIM см.: interval_updatesmeter установлен в 100
      sgraph.cnt=0;
      ct.drawImage(cv, 1,0,sgraph.width-1,cv.height, 0,0,sgraph.width-1,cv.height);  // сдвиг графика на 1px влево
      var t=new Date().getTime();
      if (v>=72) v=600;				// PA0SIM 10 минут — вертикальные линии
      else if (v>=24) v=300;		// PA0SIM 5 минут
      else if (v>=12) v=60;		// PA0SIM 1 минута
      else v=5;						// PA0SIM 5 секунд
      if (Math.floor(t/1000/v)!=Math.floor(sgraph.prevt/1000/v)) {
         // серая вертикальная линия — отметка времени
         ct.fillStyle="rgba(180,180,180,1)";			// PA0SIM темнее
         ct.fillRect(sgraph.width-1,0,1,cv.height);
         sgraph.prevt=t;
      } else {
         // белая вертикальная линия с серыми делениями шкалы дБ
         ct.fillStyle="white";
         ct.fillRect(sgraph.width-1,0,1,cv.height);
         ct.fillStyle="rgba(180,180,180,1)";			// PA0SIM темнее
         var w=sgraph.e0;
         while ((w=10*Math.ceil(w/10))<=sgraph.e1) {
            var y=s2y(w);
            ct.fillRect(sgraph.width-1,y,1,1);
            w+=1;
         }
      }
   }

   // сама точка данных
   ct.fillStyle="blue";
   ct.fillRect(sgraph.width-1,s2y(s),1,1);
}

// отдельная функция «шум» (вызывается реже) — обновляет минимум S-метра
function getnoise()
{
	try {
     	var n=soundapplet.smeter();
    	    } catch (e) { n=0; };

	smetermintimer--;
   	if ((smetermin>n-0.1) || (smetermintimer<=0))
    	{
		smetermin=n;
		if (n==0) smetermintimer=2;else
		{	if (mode=="CW")	{smetermintimer=20;}else
			if (mode=="AM")	{smetermintimer=200;}else
                        if (mode=="FM") {smetermintimer=200;}else
				{smetermintimer=40;}
		}

		if (smetermin>=0) smeterminobj.style.width= (smetermin*0.0191667)*1.09 +"px";
		else smeterminobj.style.width="0px";
		}
}

// ---------------------------------------------------------------------------
// РАЗДЕЛ 17. Список слушателей («кто слушает»)
// ---------------------------------------------------------------------------
var uu_names=new Array();
var uu_bands=new Array();
var uu_freqs=new Array();
var others_colours=[ "#ff4040", "#ffa000", "#a0a000", "#80ff00", "#00ff00", "#00a0a0", "#0080ff", "#ff40ff"];

var dxs=[];

function uu(i, username, band, freq)
{
   uu_names[i]=username;
   uu_bands[i]=band;
   uu_freqs[i]=freq;
}

var uu_compactview=false;
function douu()
{
   s='';
   total=0;
   for (b=0;b<nbands;b++) {
      if (!uu_compactview) {
         s+="<p><div  style='width:1024px; background-color:black;border-radius: 4px;box-shadow: 4px 4px 15px 0px rgba(0,0,0);margin: 0px 5px 0px 5px;margin-left: auto;margin-right: auto;'><div class=others>";
         for (i=0;i<uu_names.length;i++) if (uu_bands[i]==b && uu_names[i]!="") {
	    cbandfreq=(bandinfo[b].centerfreq-(bandinfo[b].samplerate/2)-((hi+lo)/2));
            s+="<div id='user"+i+"' align='center' style='position:relative;left:"+(uu_freqs[i]*1024-250)+"px;width:500px; color:"+others_colours[i%8]+";'>";

           s+="<button type='button' class='userfreqbtn' onclick='setfreqb("+(uu_freqs[i]*bandinfo[b].samplerate+cbandfreq).toFixed(2)+")' style='color:"+others_colours[i%8]+";'>";

	    s+="<b>"+uu_names[i]+' '+(uu_freqs[i]*bandinfo[b].samplerate+cbandfreq).toFixed(0)+"</b>";
	    s+='</button></div>';
            total++;
         }
         s+="<img src="+bi[b].scaleimgs[0][0]+"></div></div></p>";
      } else {
         s+="<p><div style='width:1024px;height:35px;position:relative; background-color:black;border-radius: 4px;box-shadow: 4px 4px 15px 0px rgba(0,0,0);margin: 0px 5px 0px 5px;margin-left: auto;margin-right: auto;'>";
         for (i=0;i<uu_names.length;i++) if (uu_bands[i]==b && uu_names[i]!="") {
            s+="<div id='user"+i+"' style='position:absolute;top:1px;left:"+
                 (uu_freqs[i]*1024)
                 +"px;width:1px;height:13px; background-color:"+others_colours[i%8]+";'></div>";
            total++;
         }
         s+="<div style='position:absolute;bottom:1px'><img src="+bi[b].scaleimgs[0][0]+"></div></div></p>";
      }
   }
   usersobj.innerHTML=s;
   numusersobj.innerHTML=total;
}

function setcompactview(c)
{
   uu_compactview=c;
   douu();
}

// AJAX-опрос списка слушателей каждые 1с
function ajaxFunction3()
{
  var xmlHttp;
  try { xmlHttp=new XMLHttpRequest(); }
    catch (e) { try { xmlHttp=new ActiveXObject("Msxml2.XMLHTTP"); }
      catch (e) { try { xmlHttp=new ActiveXObject("Microsoft.XMLHTTP"); }
        catch (e) { alert("Your browser does not support AJAX!"); return false; } } }
  xmlHttp.onreadystatechange=function()
    {
    if(xmlHttp.readyState==4)
      {
        if (xmlHttp.status==200 && xmlHttp.responseText!="") {
          eval(xmlHttp.responseText);
          douu();
        }
        clearTimeout(interval_ajax3);
        interval_ajax3 = setTimeout('ajaxFunction3()',1000);
      }
    }
  interval_ajax3 = setTimeout('ajaxFunction3()',120000);
  var url="/~~othersjj?chseq="+chseq;
  xmlHttp.open("GET",url,true);
  xmlHttp.send(null);
}

// ---------------------------------------------------------------------------
// РАЗДЕЛ 18. Проверка занятости сервера
// ---------------------------------------------------------------------------
// (Java-апплеты давно не используются — пометка html5javawarn/javatest удалена.)

function test_serverbusy()
{
   try { soundapplet.app.l=1; } catch (e) {};
   try { serveravailable=soundapplet.getid(); } catch (e) {};
   if (serveravailable==0) {
      try { clearInterval(interval_updatesmeter); } catch (e) {} ;
      try { clearTimeout(interval_ajax3); } catch (e) {} ;
      var i;
      try { for (i=0;i<nwaterfalls;i++) waterfallapplet[i].destroy(); } catch (e) {} ;
      try { soundapplet.destroy(); } catch (e) {};
      document.body.innerHTML="Sorry, the WebSDR server is too busy right now; please try again later.\n";
   }
}

// ---------------------------------------------------------------------------
// РАЗДЕЛ 19. Полоса пропускания (updbw) и куки
// ---------------------------------------------------------------------------
function updbw()
{
   if (lo>hi) {
      if (document.onmousemove == useMouseXYloweredge || touchingLower) lo=hi;
      else hi=lo;
   }
   var maxf=(mode=="FM") ? 15 : (bandinfo[band].maxlinbw*0.95);
   if (lo<-maxf) lo=-maxf;
   if (hi>maxf) hi=maxf;

   var xlo=document.getElementById('numericalfilterlow');
   var xhi=document.getElementById('numericalfilterhigh');
   xlo.innerHTML=(lo).toFixed(2);
   xhi.innerHTML=(hi).toFixed(2);

   var x6=document.getElementById('numericalbandwidth6');
   var x60=document.getElementById('numericalbandwidth60');
   x6.innerHTML=(hi-lo+0.091).toFixed(2);
   x60.innerHTML=(hi-lo+0.551).toFixed(2);

   try {
      document.getElementById('btn-'+mode).classList.add('btn-selected');
      var ar=['AM','FM','USB','LSB','CW','AMSYNC','AMCOSTAS','AMN','FMN','USBN','LSBN','CWN'];
      for (var i=0;i<ar.length;i++) if (ar[i]!=mode) document.getElementById('btn-'+ar[i]).classList.remove('btn-selected');
   } catch(e) {};

   setfreq(freq);

   pushButton(mode, lo, hi);
}

function createCookie(name,value,days) {
	if (days) {
		var date = new Date();
		date.setTime(date.getTime()+(days*24*60*60*1000));
		var expires = "; expires="+date.toGMTString();
	}
	else var expires = "";
	document.cookie = name+"="+value+expires+"; path=/";
}

function readCookie(name) {
	var nameEQ = name + "=";
	var ca = document.cookie.split(';');
	for(var i=0;i < ca.length;i++) {
		var c = ca[i];
		while (c.charAt(0)==' ') c = c.substring(1,c.length);
		if (c.indexOf(nameEQ) == 0) return c.substring(nameEQ.length,c.length);
	}
	return null;
}

// ---------------------------------------------------------------------------
// РАЗДЕЛ 20. Идентификаторы бэндов и скорость/высота водопада
// ---------------------------------------------------------------------------
// В режиме «один бэнд» id==0 для всех бэндов (band2id), а id2band возвращает
// текущий выбранный. В режиме «все бэнды» id==band.

function id2band(id)
{
   if (view == Views.oneband) return band; else return id;
}

function band2id(b)
{
   if (view == Views.oneband) return 0; else return b;
}

function waterfallspeed(sp)
{
   waterslowness=sp;
   if (waitingforwaterfalls>0) return;
   var done=0;
   if (view==Views.othersslow) {
      for (i=0;i<nwaterfalls;i++)
         if (i==band) waterfallapplet[i].setslow(sp);
         else waterfallapplet[i].setslow(100);
   } else {
      for (i=0;i<nwaterfalls;i++)
         waterfallapplet[i].setslow(sp);
   }
}

function waterfallheight(si)
{
   waterheight=si;
   var wfs=document.getElementById('wf-size');
   if (wfs) wfs.value=si;
   if (waitingforwaterfalls>0) return;
   for (i=0;i<nwaterfalls;i++) {
      waterfallapplet[i].setSize(1024,si);
   }

   var y=scaleobj.offsetTop+15;
   passbandobj.style.top=y+"px";
   edgelowerobj.style.top=y+"px";
   edgeupperobj.style.top=y+"px";
   carrierobj.style.top=y+"px";
}

function waterfallmode(m)
{
   watermode=m;
   if (waitingforwaterfalls>0) return;
   for (i=0;i<nwaterfalls;i++) {
      waterfallapplet[i].setmode(m);
   }
}

// ---------------------------------------------------------------------------
// РАЗДЕЛ 21. Готовность водопада и звука
// ---------------------------------------------------------------------------
// soundappletstarted() вызывается звуковым апплетом когда WebSocket открыт;
// waterfallappletstarted() — водопадным апплетом после создания канвасов.

function soundappletstarted()
{
   setTimeout('soundappletstarted2()',100);
}

function soundappletstarted2()
{
   allloadeddone=true;

   soundapplet.setvolume(Math.pow(10, document.getElementById('volumecontrol2').value /10.));

   if (bi[0]) {
      setfreqif(freq);
      updbw();
   }

   try { setmute(document.getElementById('mutecheckbox').checked) } catch(e){};
   try { toggle_squelch(document.getElementById('gainlevelcheckbox').checked) } catch(e){};
    soundapplet.smetercallback = function(val) {
        if (!ab_squelch || !soundapplet) return;

        var threshold_db = parseInt(document.getElementById('manualgain').value);
        if (isNaN(threshold_db)) threshold_db = 20;

        var thr = smetermin + threshold_db * 100;
        if (thr <= 0) return;

        var hysteresis = SQL_HYSTERESIS_DB * 100;

        if (squelch_open) {
            if (val < thr - hysteresis) {
                if (!squelch_hang_timer) {
                    squelch_hang_timer = setTimeout(function() {
                        squelch_open = false;
                        soundapplet.setmute(true);
                        squelch_hang_timer = null;
                    }, SQL_HANG_MS);
                }
            } else {
                if (squelch_hang_timer) {
                    clearTimeout(squelch_hang_timer);
                    squelch_hang_timer = null;
                }
            }
        } else {
            if (val >= thr + hysteresis) {
                squelch_open = true;
                soundapplet.setmute(false);
            }
        }
    };
   try { setautonotch(document.getElementById('autonotchcheckbox').checked) } catch(e){};

   test_serverbusy();
}

function waterfallappletstarted(id)
{
   waitingforwaterfalls--;
   if (waitingforwaterfalls<0) waitingforwaterfalls=0;
   if (waitingforwaterfalls!=0) return;
   setTimeout('allwaterfallappletsstarted()',100);
}

function allwaterfallappletsstarted()
{
   var i;

   waterfallspeed(waterslowness);
   waterfallmode(watermode);

   for (i=0;i<nwaterfalls;i++) {
      var e=bi[i];
      waterfallapplet[i].setband(e.realband, e.maxzoom, e.zoom, e.start);
   }
   if (view==Views.oneband) {
      var e=bi[band];
      waterfallapplet[0].setband(band, e.maxzoom, e.zoom, e.start);
   }
    for (i=0;i<nwaterfalls;i++) {
     scaleobjs[i] = document.getElementById('clipscale'+i);
     scaleimgs0[i] = document.images["s0cale"+i];
     scaleimgs1[i] = document.images["s1cale"+i];
   }
   if (view==Views.oneband) {
      setscaleimgs(band,0);
      scaleobj = scaleobjs[0];
   } else {
      for (i=0;i<nwaterfalls;i++) setscaleimgs(i,i);
      scaleobj=scaleobjs[band];
   }
   draw_passband();
   for (var i=0;i<nwaterfalls;i++) if (!hidedx) showdx(id2band(i));
}

// ---------------------------------------------------------------------------
// РАЗДЕЛ 23. Обработка мыши и тача
// ---------------------------------------------------------------------------
function registerTouchEvents(id, touchStart, touchMove) {
   var elem=document.getElementById(id);
   elem.addEventListener('touchstart', touchStart);
   elem.addEventListener('touchmove', touchMove);
   elem.addEventListener('touchend', touchEnd);
}

function setusernamecookie() {

   if (document.usernameform.username.value.length > 3 || /\s+/.test(document.usernameform.username.value))
   {
     ip2geo('visited');
     document.usernameform.username.value="";
   }

   createCookie('username',document.usernameform.username.value,365*5);
   var p=document.getElementById("please1");
   if (p) p.innerHTML="Пожалуйста, введите имя или позывной (сохраняется в куки): ";
   p=document.getElementById("please2");
   if (p) p.innerHTML="";
   send_soundsettings_to_server();
}

var dragging=false;
var dragorigX;
var dragorigval;
var touchingLower=false;

function getMouseXY(e)
{
   e = e || window.event;
   if (e.pageX || e.pageY) return {x:e.pageX, y:e.pageY};
   return {
     x:e.clientX + document.body.scrollLeft - document.body.clientLeft,
     y:e.clientY + document.body.scrollTop  - document.body.clientTop
   };
}

function useMouseXY(e)
{
   var pos=getMouseXY(e);
   var coords = scaleobj.offsetParent.getBoundingClientRect()
   setfreq((pos.x-coords.left-512)*khzperpixel+centerfreq-(hi+lo)/2);
   return cancelEvent(e);
}

function touchXY(ev)
{
   ev.preventDefault();
   for (var i=0; i<ev.touches.length; i++) {
      var x = ev.touches[i].pageX;
      setfreq((x-scaleobj.offsetParent.offsetLeft-512)*khzperpixel+centerfreq-(hi+lo)/2);
   }
}

function useMouseXYloweredge(e)
{
   var pos=getMouseXY(e);
   lo=dragorigval+(pos.x-dragorigX)*khzperpixel;
   updbw();
   return cancelEvent(e);
}

function touchXYloweredge(ev)
{
   ev.preventDefault();
   for (var i=0; i<ev.touches.length; i++) {
      var x = ev.touches[i].pageX;
      lo=dragorigval+(x-dragorigX)*khzperpixel;
      updbw();
   }
}

function useMouseXYupperedge(e)
{
   var pos=getMouseXY(e);
   hi=dragorigval+(pos.x-dragorigX)*khzperpixel;
   updbw();
   return cancelEvent(e);
}

function touchXYupperedge(ev)
{
   ev.preventDefault();
   for (var i=0; i<ev.touches.length; i++) {
      var x = ev.touches[i].pageX;
      hi=dragorigval+(x-dragorigX)*khzperpixel;
      updbw();
   }
}

function useMouseXYpassband(e)
{
   var pos=getMouseXY(e);
   setfreq(dragorigval+(pos.x-dragorigX)*khzperpixel);
   return cancelEvent(e);
}

function touchXYpassband(ev)
{
   ev.preventDefault();
   for (var i=0; i<ev.touches.length; i++) {
      var x = ev.touches[i].pageX;
      setfreq(dragorigval+(x-dragorigX)*khzperpixel);
   }
}

function mouseup(e)
{
   if (dragging) {
      dragging=false;
      document.onmousemove(e);
      document.onmousemove = null;
   }
}

function touchEnd(ev) {
   ev.preventDefault();
   if (dragging) {
      dragging=false;
      touchingLower=false;
   }
}

function imgmousedown(ev,bb)
{
   var b=id2band(bb);
   dragging=true;
   document.onmousemove = useMouseXY;
   if (view!=Views.oneband && band!=b) {
      if (view==Views.othersslow) waterfallspeed(waterslowness);
      setband(b);
      useMouseXY(ev);
   }
}

function imgtouch(ev) {
   ev.preventDefault();

   var e = ev || window.event;
   var img;
   if (e.target) img = e.target; else
   if (e.srcElement) img = e.srcElement;
   if (img.nodeType == 3) img = img.parentNode;
   var bb=0;
   if (img.name) bb = img.name.substring(6,7); else
   if (img.id) bb = img.id.substring(8,9);

   var b=id2band(bb);
   if (view!=Views.oneband && band!=b) {
      if (view==Views.othersslow) waterfallspeed(waterslowness);
      setband(b);
   }

   if (ev.targetTouches.length == 1) {
      dragging=true;
      dragorigX=ev.targetTouches[0].pageX;
      touchXY(ev);
   }
}

function mousedownlower(ev)
{
   var pos=getMouseXY(ev);
   dragging=true;
   document.onmousemove = useMouseXYloweredge;
   dragorigX=pos.x;
   dragorigval=lo;
   return cancelEvent(ev);
}

function touchlower(ev) {
   ev.preventDefault();
   if (ev.targetTouches.length == 1) {
      touchingLower=true;
      dragging=true;
      dragorigX=ev.targetTouches[0].pageX;
      dragorigval=lo;
   }
}

function mousedownupper(ev)
{
   var pos=getMouseXY(ev);
   dragging=true;
   document.onmousemove = useMouseXYupperedge;
   dragorigX=pos.x;
   dragorigval=hi;
   return cancelEvent(ev);
}

function touchupper(ev) {
   ev.preventDefault();
   if (ev.targetTouches.length == 1) {
      dragging=true;
      dragorigX=ev.targetTouches[0].pageX;
      dragorigval=hi;
   }
}

function mousedownpassband(ev)
{
   var pos=getMouseXY(ev);
   dragging=true;
   document.onmousemove = useMouseXYpassband;
   dragorigX=pos.x;
   dragorigval=freq;
   return cancelEvent(ev);
}

function touchpassband(ev) {
   ev.preventDefault();
   if (ev.targetTouches.length == 1) {
      dragging=true;
      dragorigX=ev.targetTouches[0].pageX;
      dragorigval=freq;
   }
}

function docmousedown(ev)
{
   var fobj;
   if (!ev) fobj=event.srcElement;  // IE
   else fobj = ev.target;  // FF
   if (fobj.className == "scale" || fobj.className=="scaleabs") return cancelEvent(ev);
   return true;
}

var tprevwheel=0;
var prevdir=0;
var wheelstep=1000;
function mousewheel(ev)
{
   var fobj;
   if (!ev) {
      ev=window.event; fobj=event.srcElement;	// IE
   }
   else fobj = ev.target;	// FF/IE9

   if (navigator.platform.substring(0,3)=="Win" && fobj.tagName=='APPLET' && fobj.name.substring(0,15)=="waterfallapplet") {
         var pos=getMouseXY(ev);
         var x=pos.x-fobj.offsetParent.offsetLeft;
         if (ev.wheelDelta>0) document[fobj.name].setzoom(-2, x);
         else if (ev.wheelDelta<0) document[fobj.name].setzoom(-1, x);
         return cancelEvent(ev);
   }

   if (fobj.nodeType==3) fobj=fobj.parentNode;	// 3=TEXT_NODE

   if (fobj.className == "scale" || fobj.className=="scaleabs" || fobj.className.substring(0,8) == "statinfo") {
      // настройка колесом мыши на шкале
      var delta = ev.detail ? ev.detail : ev.wheelDelta/-40;
      var t=new Date().getTime();
      var dt=t-tprevwheel;
      if (dt<10) dt=10;
      tprevwheel=t;
      prevdir=delta;
      if (Math.abs(delta)<wheelstep && delta!=0) wheelstep=Math.abs(delta);
      delta/=wheelstep;
      if (prevdir*delta>0 && dt<500) delta*=(500./dt);
      setfreq(freq-delta/20);
      return cancelEvent(ev);
   }

   return true;
}

if (document.addEventListener) {
  window.addEventListener('DOMMouseScroll', mousewheel, false);
  document.addEventListener('mousewheel', mousewheel, false);
//  document.addEventListener('wheel', mousewheel, false);    // современные браузеры используют 'wheel', но старые нет; пока оставлено как было
  window.addEventListener('mouseup', mouseup, false);
  window.addEventListener('mousedown', docmousedown, false);
} else {
  window.onmousewheel = mousewheel;
  document.onmousewheel = mousewheel;
  document.onmouseup = mouseup;
  document.onmousedown = docmousedown;
}

// ---------------------------------------------------------------------------
// РАЗДЕЛ 24. Управление клавиатурой
// ---------------------------------------------------------------------------
var allowkeyboard;

function keydown(e)
{
   if (!document.viewform.allowkeys.checked) return true;
   e = e ? e : window.event;
   if (!e.target) e.target = e.srcElement;
   if (e.target.nodeName=="INPUT" && e.target.type=="text" && e.target.name!="frequency") return true;  // не перехватываем ввод в текстовых полях (кроме частоты)
   var st=1;
   if (e.shiftKey) st=2;
   if (e.ctrlKey || e.altKey || e.metaKey) st=3;
   switch (e.keyCode) {
      case 37:                                                         // left arrow
      case 74: freqstep(-st);                return cancelEvent(e);    // J
      case 39:                                                         // right arrow
      case 75: freqstep(st);                 return cancelEvent(e);    // K
      case 65: setmf ('am',  -4.96 ,  4.95  );    return cancelEvent(e);    // A
      case 70: setmf ('fm',  -6.2  ,  6.21  );    return cancelEvent(e);    // F
      case 67: setmf ('cw', -0.95, -0.55);   return cancelEvent(e);    // C
      case 76: setmf('lsb', -2.76, -0.15);     return cancelEvent(e);    // L
      case 85: setmf('usb',  0.15,  2.76);     return cancelEvent(e);    // U
	  case 77:   // M = mute
          var mm=!document.getElementById("mutecheckbox").checked;
          document.getElementById("mutecheckbox").checked=mm;
          setmute(mm);
		  toggle_info('mute', mm);
          return cancelEvent(e);
      case 86:   // V/v = громкость
          var vv=document.getElementById("volumecontrol2").value;
          if (e.shiftKey) vv++; else vv--;
          document.getElementById("volumecontrol2").value=vv;
		  document.getElementById("volumedb").textContent=vv.toString()+'dB';
          set_volume(vv);
          return cancelEvent(e);
	case 87:   // w/W = уже/шире
			  if (e.shiftKey) {
				 if (lo<0) lo*=1.1; else lo/=1.1; if (hi>0) hi*=1.1; else hi/=1.1; updbw();
			  } else {
				 if (lo>0) lo*=1.1; else lo/=1.1; if (hi<0) hi*=1.1; else hi/=1.1; updbw();
			  }
			  return cancelEvent(e);

      case 90: if (e.shiftKey) wfset(2); else wfset(4); return cancelEvent(e);   // Z
      case 71: document.freqform.frequency.value=""; document.freqform.frequency.focus(); return cancelEvent(e);    // G
      case 66: if (e.shiftKey) setband((band-1+nbands)%nbands);        // B
               else setband((band+1)%nbands);
               return cancelEvent(e);
   }
   return true;
}

window.onkeydown = keydown;

// ---------------------------------------------------------------------------
// РАЗДЕЛ 25. Построение GUI динамически
// ---------------------------------------------------------------------------
// visit/newid/document_username — подстановка гео-имени в поле ввода имени.
// document_bandbuttons — кнопки бэндов; document_waterfalls — контейнеры
// водопадов; document_soundapplet — звуковой апплет; stretch_waterfalls —
// растяжение водопада на всю ширину.

function visit(tmpid) {
  if ( document.getElementById(tmpid).value == '') {
    document.getElementById(tmpid).value = document.getElementById(tmpid).value + " " + geo;
    document.usernameform.username.value = document.getElementById(tmpid).value;
  } else {
    document.getElementById(tmpid).value = document.getElementById(tmpid).value;
    if (document.getElementById(tmpid).value.length > 10 || /\s+/.test(document.getElementById(tmpid).value))
    {
      ip2geo('visited');
      document.getElementById(tmpid).value = document.getElementById(tmpid).value + " " + geo;
    }
    document.usernameform.username.value = document.getElementById(tmpid).value;
  }
}

function newid(tmpid) {
  document.getElementById(tmpid).value = document.getElementById(tmpid).value + " " + geo;
  document.usernameform.username.value = document.getElementById(tmpid).value;
}

function document_username()
{
  var x= readCookie('name');

  if (x) {
    document.write('<span id="please">Пожалуйста, введите имя или позывной (сохраняется в куки): ');
    document.write('<input type="text" id="visited" name="name" value="" ondragstart="return false" ondrop="return false" ondrag="return false" onpaste="return false" maxlength="6" onblur="visit(this.id); setusernamecookie();" onclick=""></span>');
   document.write('<span id="please4">         ' );

    if (x.length > 6 || /\s+/.test(document.usernameform.username.value))
    {
      ip2geo('visited');
      x="";
    }

    document.usernameform.username.value=x;
  } else {
    document.write('<span id="please"><span id="please1"><b><i>Пожалуйста, введите имя или позывной :<\/i><\/b></span> ');
    document.write('<input type="text" id="time" name="username" value="" ondragstart="return false" ondrop="return false" ondrag="return false" onpaste="return false" maxlength="6" onfocus=this.value="" onblur="visit(this.id); setusernamecookie();" onclick=""></span>');

    ip2geo('time');
  }
}

function document_bandbuttons() {
   var bb=document.getElementById('bandbuttons');
   if (!bb) return;
   /* bodyonload теперь вызывается один раз (window.onload из <body onload>);
    *  хук $(document).ready(bodyonload) в websdr-head.html был удалён, потому
    *  что гонялся с асинхронной загрузкой websdr-waterfall.js/websdr-sound.js
    *  и ломал init. Защита оставлена: setview() перезапускает это на смене
    *  видов, и пересборка innerHTML стёрла бы подсветку .btn-selected,
    *  применённую setfreqif/setband во время первого прохода. */
   if (bb.children.length > 0) return;
   /* Подпись кнопки бэнда: имя конфигурации ASCII (bandinfo `name`),
    *  но бэнд-зуммер 4625 кГц отображается на кнопке как «УВБ». */
   var bandlabel=function(n){ return n=='UVB' ? 'УВБ' : n; };
   var s='';
   for (var i=0;i<nbands;i++) {
      var b=bi[i];
      s+='<button type="button" name="group0" id="btnB-'+i+'" class="btnBand" onclick="setband('+i+');">'+bandlabel(b.name)+'</button>';
      if ((i+1)%4==0) s+='<br />';
   }
   /* Диапазонные кнопки-ссылки (cfg "buttonlink <метка>|<URL>") — ведут на
    * внешние rx-WebSDR серверы (не бэнды этого приёмника), открываются в
    * новой вкладке. Массив buttonlinks приходит в bandinfo.js от сервера.
    * Кнопка (а не <a>): .btnBand задан под <button> (display:inline-block),
    * у <a> width/height не применяются и элемент вырождается в «кружок». */
   if (typeof buttonlinks != 'undefined') {
      for (var li=0; li<buttonlinks.length; li++) {
         var bl=buttonlinks[li];
         s+='<button type="button" class="btnBand" onclick="window.open(\''+bl.url+'\',\'_blank\')" title="'+bl.label+'">'+bl.label+'</button>';
         if ((nbands+li+1)%4==0) s+='<br />';
      }
   }
   bb.innerHTML=s;
}

function document_waterfalls()
{
  if (view==Views.allbands || view==Views.othersslow) nwaterfalls=nvbands;
  else if (view==Views.oneband) nwaterfalls=1;
  else {
     nwaterfalls=0;
     document.getElementById('waterfalls').innerHTML="";
     return;
  }

  var i;
  var b;
  var s="";
  for (i=0;i<nwaterfalls;i++) {
    b = id2band(i);
    e=bi[b];
    j=e.realband;
    s+=
      '<div id="wfdiv'+i+'"></div>'+
      '<div class="scale" style="overflow:hidden; width:1024px; height:'+scaleheight+'px; position:relative" title="click to tune" id="clipscale'+i+'" onmousedown="return false">' +
        '<img src="'+e.scaleimgs[0]+'" onmousedown="imgmousedown(event,'+i+')" class="scaleabs" style="top:0px" name="s0cale'+i+'">' +
        '<img src="'+e.scaleimgs[0]+'" onmousedown="imgmousedown(event,'+i+')" class="scaleabs" style="top:0px" name="s1cale'+i+'">' +
      '</div>' +
      '<div class="scale" style="width:1024px;height:20px;background-color:black;position:relative;" id="blackbar'+i+'" title="click to tune" onmousedown="imgmousedown(event,'+i+')"><\/div>' +
      '\n';
     waterfallapplet[i]={};
     waterfallapplet[i].div='wfdiv'+i;
     waterfallapplet[i].id=i;
     waterfallapplet[i].band=b;
     waterfallapplet[i].maxzoom=bi[b].maxzoom;
  }

  waitingforwaterfalls=nwaterfalls;     // это должно быть ДО следующей строки, чтобы избежать гонки
  document.getElementById('waterfalls').innerHTML=s;

  // HTML5-водопад всегда (Java-апплеты не поддерживаются браузерами;
  // выбор «Java» в меню html5orjavamenu удалён как рудимент).
  if (typeof prep_html5waterfalls =="function") prep_html5waterfalls();
  else {
     script = document.createElement('script');
     script.src = 'rx-waterfall.js';
     script.type = 'text/javascript';
     document.body.appendChild(script);
  }

  for (i=0;i<nwaterfalls;i++) {
    scaleobjs[i] = document.getElementById('clipscale'+i);
    scaleimgs0[i] = document.images["s0cale"+i];
    scaleimgs1[i] = document.images["s1cale"+i];
    if (isTouchDev) {
       registerTouchEvents('clipscale'+i, imgtouch, touchXY);
       registerTouchEvents('blackbar'+i, imgtouch, touchXY);
    }
  }
}

function document_soundapplet() {
  // HTML5-звук всегда (см. комментарий про Java в document_waterfalls).
  if (typeof prep_html5sound =="function") prep_html5sound();
  else {
     script = document.createElement('script');
     script.src = 'websdr-sound.js';
     script.type = 'text/javascript';
     document.body.appendChild(script);
  }
}

function stretch_waterfalls()
{
   setTimeout('stretch_waterfalls_do()',1);
}

function stretch_waterfalls_do()
{
  var wfc=document.getElementById('wfcontainer');
  var wfcc=document.getElementById('wfccontainer');
  // флажок «широкий водопад» может ещё не существовать при первом resize
  // (страница в процессе отрисовки) — пропускаем такой вызов
  var wfwide=document.getElementById('wfwidecheckbox');
  if (!wfwide) return;

  if (!wfwide.checked || usejavawaterfall) {
    wfc.style.transform="";
    wfc.style.left="0px";
    wfc.style.width="";
    wfc.style.height="";
    wfcc.style.height="";
    wfscalex=1;
    return;
  }
  var w=document.body.offsetWidth || window.innerWidth;
  var style = document.body.currentStyle || window.getComputedStyle(document.body);
  var marginleft = parseFloat(style.marginLeft)-8;
  var marginright = parseFloat(style.marginRight)-18;
  w+=marginleft+marginright;
  var wd=w;
  if (w>1024) wd=1024;
  if (w<1024) w=1024;
  wfscalex=w/1024;
  wfc.style.transform="scale("+(w/1024)+")";
  wfc.style.left=(-marginleft)+"px";
  wfcc.style.height=(wfc.clientHeight*wfscalex)+"px";
}

window.addEventListener('resize', stretch_waterfalls, false);

// ---------------------------------------------------------------------------
// РАЗДЕЛ 26. Запись, чат, журнал, гео, фон, прелоадер
// ---------------------------------------------------------------------------
var rec_showtimer;
var rec_downloadurl;

function record_show()
{
   document.getElementById('reccontrol').innerHTML=Math.round(soundapplet.rec_length_kB())+" kB";
}

function record_start() {
   document.getElementById('reccontrol').innerHTML=0+" kB";
   if (rec_downloadurl) { URL.revokeObjectURL(rec_downloadurl); rec_downloadurl=null; }
   rec_showtimer=setInterval('record_show()',250);
   soundapplet.rec_start();
}

function record_stop()
{
   clearInterval(rec_showtimer);
   var res = soundapplet.rec_finish();

   var wavhead = new ArrayBuffer(44);
   var dv=new DataView(wavhead);
   var i=0;
   var sr=Math.round(res.sr);
   dv.setUint8(i++,82);  dv.setUint8(i++,73); dv.setUint8(i++,70); dv.setUint8(i++,70); // RIFF
   dv.setUint32(i,res.len+44,true); i+=4;  // общая длина; WAV little-endian
   dv.setUint8(i++,87);  dv.setUint8(i++,65); dv.setUint8(i++,86); dv.setUint8(i++,69); // WAVE
   dv.setUint8(i++,102);  dv.setUint8(i++,109); dv.setUint8(i++,116); dv.setUint8(i++,32); // fmt
     dv.setUint32(i,16,true);   i+=4;   // длина fmt
     dv.setUint16(i,1,true);    i+=2;   // PCM
     dv.setUint16(i,1,true);    i+=2;   // mono
     dv.setUint32(i,sr,true);   i+=4;   // samplerate
     dv.setUint32(i,2*sr,true); i+=4;   // 2*samplerate
     dv.setUint16(i,2,true);    i+=2;   // байт на сэмпл
     dv.setUint16(i,16,true);   i+=2;   // бит на сэмпл
   dv.setUint8(i++,100);  dv.setUint8(i++,97); dv.setUint8(i++,116); dv.setUint8(i++,97); // data
     dv.setUint32(i,res.len,true);  // длина data

   var wavdata = res.wavdata;
   wavdata.unshift(wavhead);

   var mimetype = 'application/binary';
   var bb = new Blob(wavdata, {type: mimetype});
   if (!bb) document.getElementById('recwarning').style.display="block";
   rec_downloadurl = window.URL.createObjectURL(bb);
   if (rec_downloadurl.indexOf('http')>=0) document.getElementById('recwarning').style.display="block";
   var fname='';
   try {
      fname=(new Date().toISOString()).replace(/\.[0-9]{3}/,"");
   } catch (e) {};
   fname="websdr_recording_"+fname+"_"+nominalfreq().toFixed(1)+"kHz.wav";
   document.getElementById('reccontrol').innerHTML="<a href='"+rec_downloadurl+"' download='"+fname+"'>download</a>";
}

function record_click()
{
   var bt=document.getElementById('recbutton');
   if (bt.innerHTML=="stop") {
      bt.innerHTML="start";
      record_stop();
   } else {
      bt.innerHTML="stop";
      record_start();
   }
}

function sendchat()
{
  timeout_idle_restart()
  var xmlHttp;
  try { xmlHttp=new XMLHttpRequest(); }
    catch (e) { try { xmlHttp=new ActiveXObject("Msxml2.XMLHTTP"); }
      catch (e) { try { xmlHttp=new ActiveXObject("Microsoft.XMLHTTP"); }
        catch (e) { alert("Your browser does not support AJAX!"); return false; } } }
  var url="/~~chat";
  var msg=encodeURIComponent(document.chatform.chat.value);
  url=url+"?name="+encodeURIComponent(document.usernameform.username.value)+"&msg="+encodeURIComponent(document.chatform.chat.value);
  xmlHttp.open("GET",url,true);
  xmlHttp.send(null);
  document.chatform.chat.value="";
  return false;
}

function chatnewline(s)
{
  var o=document.getElementById('chatboxnew');
  if (!o) return;
  if (s[0]=='-') {
     var div=document.createElement('div');
     div.innerHTML=s;
     s=div.innerHTML;
     var re=new RegExp('<br>'+s.substring(1).replace(/[\-\[\]\/\{\}\(\)\*\+\?\.\\\^\$\|]/g, "\\$&")+'.*','g');
     o.innerHTML=o.innerHTML.replace(re,'<br>');
     return;
  }
  o.innerHTML+='<br>'+s+'\n';
  o.scrollTop=o.scrollHeight;
}

function sendlogclear()
{
  document.logform.comment.value="";
}

function sendlog()
{
  var xmlHttp;
  try { xmlHttp=new XMLHttpRequest(); }
    catch (e) { try { xmlHttp=new ActiveXObject("Msxml2.XMLHTTP"); }
      catch (e) { try { xmlHttp=new ActiveXObject("Microsoft.XMLHTTP"); }
        catch (e) { alert("Your browser does not support AJAX!"); return false; } } }
  var url="/~~loginsert";
  url=url
     +"?name="+encodeURIComponent(document.usernameform.username.value)
     +"&freq="+nominalfreq()
     +"&call="+encodeURIComponent(document.logform.call.value)
     +"&comment="+encodeURIComponent(document.logform.comment.value)
     ;
  xmlHttp.open("GET",url,true);
  xmlHttp.send(null);
  document.logform.call.value="";
  document.logform.comment.value="";
  xmlHttp.onreadystatechange=function()
    {
    if(xmlHttp.readyState==4)
      {
      document.logform.comment.value=xmlHttp.responseText;
      }
    }
  setTimeout("document.logform.comment.value=''",1000);
  return false;
}

function ip2geo(id)
{
  var xhttp = new XMLHttpRequest();

  xhttp.open("GET","http://ip-api.com/csv?fields=countryCode,city", true);
  xhttp.send();
  xhttp.onreadystatechange = function()
  {
    if (xhttp.readyState == 4 && xhttp.status == 200) { geo = xhttp.responseText, document.getElementById(id).value = geo }
    else { document.getElementById(id).value = ("unknown") }
    setTimeout( function() {if (document.getElementById(id).value == "" ) {window.location.href="access.html"} }, 1211);
    setTimeout( function() {if (document.getElementById(id).value.indexOf("::ffff_") >=0 ) {window.location.href="access.html"} }, 1213);
    setTimeout( function() {if (document.getElementById(id).value.indexOf("undefined") >=0 ) {window.location.href="access.html"} }, 1214);
    setTimeout( function() {if (document.getElementById(id).value.indexOf("invalid") >=0 ) {window.location.href="access.html"} }, 1215);
    setTimeout( function() {if (document.getElementById(id).value.indexOf("error") >=0 ) {window.location.href="access.html"} }, 1216);
    setTimeout( function() { document.usernameform.username.value = document.getElementById(id).value }, 500);
    setTimeout( function() { document.usernameform.username.value = document.getElementById(id).value }, 1400);
  }
}

function debug(a)
{
   console.log(a);
}

function toggle_info (info_type, info_mode='LSB')
{
	e = document.getElementById(info_type+"_info");
	if  (info_type=='mode') {
		e.textContent=info_mode
	}
	else if (info_type=='nr' && info_mode==0) {
		document.getElementById(info_type+"_info").classList.remove('is_on');
	}
	else if (info_type=='agc') {
		if (e.className.includes('is_off') && info_type!=='nr') {
			document.getElementById(info_type+"_info").classList.remove('is_off');
		}
		else  {
			document.getElementById(info_type+"_info").classList.add('is_off');
		}
	}
	else {
		// info_mode дублирует желаемое состояние ON/OFF для mobile-toggle чекбоксов.
		// Когда передаётся boolean — устанавливаем индикатор точно (без слепого
		// переключения), чтобы он никогда не рассинхронизировался с чекбоксом.
		var on = (typeof info_mode === 'boolean') ? info_mode
		                                        : !e.className.includes('is_on');
		if (on) {
			document.getElementById(info_type+"_info").classList.add('is_on');
		}
		else  {
			document.getElementById(info_type+"_info").classList.remove('is_on');
		}
	}
}

function background_load()
{
	var on = document.getElementById('background_toggle').checked;
	if (on) {
		document.body.style.background = 'url(bg6.jpg) no-repeat center center fixed';
		document.body.style.backgroundSize= 'cover';
	}
	else {
		document.body.style.background ='#bedcd7'
	}
	settings_store();
}

function preloader () {
	document.getElementsByClassName('loader')[0].classList.add('loaded_hiding');
	window.setTimeout(function () {
      document.getElementsByClassName('loader')[0].classList.add('loaded');
      document.getElementsByClassName('loader')[0].remove('loaded_hiding');
    }, 700);
}

  $(window).on('load', function() {
	window.setTimeout(preloader, 500)
  }
 );
