// Demo mode for the web demo (GitHub Pages). The same pages as the app and the logger, but
// every /api/ request is answered from demo/data.json: a copy of real flights taken from a
// logger. Changes (names, photos, service hours, deleting) last until the page is reloaded.
window.JCL_DEMO=true;
(function(){
  let data=null, loading=null;
  const load=()=>data?Promise.resolve(data):(loading||(loading=window.__realFetch("data.json",{cache:"no-store"}).then(r=>r.json()).then(d=>(data=d))));
  window.__realFetch=window.fetch.bind(window);
  const json=(b,s)=>new Response(JSON.stringify(b),{status:s||200,headers:{"content-type":"application/json"}});
  const text=(b,s)=>new Response(b,{status:s||200,headers:{"content-type":"text/plain"}});
  const nope=()=>json({error:"Not available in the demo"},400);
  const parts={};
  const welcome="JetCat Logger demo\nThis console talks to a real logger plugged into a JetCat ECU.\nIn the demo it has nothing to talk to.\n";
  window.fetch=async(url,opts)=>{
    const u=new URL(String(url),location.href), i=u.pathname.indexOf("/api/");
    if(i<0) return window.__realFetch(url,opts);
    const p=u.pathname.slice(i), q=Object.fromEntries(u.searchParams), m=((opts&&opts.method)||"GET").toUpperCase(), d=await load();
    if(m==="GET") switch(p){
      case "/api/status.json": return json({...d.status,via:"demo",flights:d.flights.length,downloading:false,state:"Idle"});
      case "/api/flights.json": return json(d.flights);
      case "/api/helis.json": return json(d.helis);
      case "/api/heliinfo.json": return json(d.heliinfo);
      case "/api/flight.json": {const f=d.flights.find(f=>f.key===q.key);return f?json(f):json({error:"not found"},404);}
      case "/api/flight.csv": return d.csv[q.key]?text(d.csv[q.key]):json({error:"no log"},404);
      case "/api/photo": return d.photos[q.serial]?text(d.photos[q.serial]):json({error:"no photo"},404);
      case "/api/console": return json({first:0,next:welcome.length,total:welcome.length,text:+q.since>0?"":welcome});
      default: return json({error:"not found"},404);
    }
    switch(p){
      case "/api/time": return json({ok:true});
      case "/api/heli": {if(q.name)d.helis[q.serial]=q.name;else delete d.helis[q.serial];return json(d.helis);}
      case "/api/heliinfo": {
        const o=d.heliinfo[q.serial]||(d.heliinfo[q.serial]={photo_len:(d.photos[q.serial]||"").length});
        for(const k of ["service_h","interval_h"]) o[k]=q[k]!==undefined&&q[k]!==""?+q[k]:null;
        o.service_date=q.service_date||null; return json(d.heliinfo);
      }
      case "/api/photo": {
        const sr=q.serial, info=d.heliinfo[sr]||(d.heliinfo[sr]={service_h:null,service_date:null,interval_h:null});
        if(q.delete==="1"){delete d.photos[sr];info.photo_len=0;return json(d.heliinfo);}
        const body=String((opts&&opts.body)||""), chunk=body.slice(body.indexOf('"')+1,body.lastIndexOf('"'));
        parts[sr]=(+q.chunk?parts[sr]||"":"")+chunk;
        if(+q.chunk===+q.total-1){d.photos[sr]=parts[sr];info.photo_len=parts[sr].length;delete parts[sr];}
        return json({ok:true});
      }
      case "/api/delete": {d.flights=d.flights.filter(f=>f.key!==q.key);return json({ok:true});}
      case "/api/console": return json({ok:true});
      default: return nope();
    }
  };
  document.addEventListener("DOMContentLoaded",()=>{
    const b=document.createElement("div");
    b.style.cssText="background:#f59e0b;color:#111;font:600 13px/1.35 system-ui,sans-serif;padding:7px 12px;text-align:center";
    b.innerHTML="Demo · real flights from a BO105 and an Alouette, read off a JetCat Logger. Anything you change is forgotten when you reload.";
    document.body.insertBefore(b,document.body.firstChild);
  });
})();
