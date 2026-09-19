from pathlib import Path
import subprocess,yaml,re,json,time
base=Path(__file__).resolve().parent;compose=base/'docker-compose-basic-nrf-lbo-roaming.yaml'
compose_cmd=['docker','compose'] if subprocess.run(['docker','compose','version'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0 else ['docker-compose']
c=yaml.safe_load(compose.read_text());names=['oai-sepp-A','oai-sepp-B'];shared=set(c['services'][names[0]]['networks']) & set(c['services'][names[1]]['networks']);assert len(shared)==1
roaming=shared.pop();changed=False
for name in names:
 s=c['services'][name];networks=s['networks'];local=next(n for n in networks if n!=roaming)
 output=subprocess.check_output(['docker','exec',s['container_name'],'ifconfig'],text=True)
 interfaces={ip:iface for iface,ip in re.findall(r'(?m)^(\w+):[^\n]*\n\s+inet ([0-9.]+)',output)}
 wanted={'sbi':interfaces[networks[local]['ipv4_address']],'nbi':interfaces[networks[roaming]['ipv4_address']]}
 mount=next(v for v in s['volumes'] if v.endswith(':/openair-sepp/etc/config.yaml'));p=(base/mount.split(':')[0]).resolve();text=p.read_text();root=yaml.compose(text);nfs=next(v for k,v in root.value if k.value=='nfs');sepp=next(v for k,v in nfs.value if k.value=='sepp');edits=[]
 for k,v in sepp.value:
  if k.value in wanted:
   i=next(vv for kk,vv in v.value if kk.value=='interface_name')
   if i.value!=wanted[k.value]:edits.append((i.start_mark.index,i.end_mark.index,wanted[k.value]))
 for a,b,value in sorted(edits,reverse=True):text=text[:a]+value+text[b:]
 if edits:p.write_text(text);changed=True
 print(name,wanted,'updated' if edits else 'correct')
if changed:
 for name in reversed(names):
  subprocess.run(compose_cmd+['-f',str(compose),'restart',name],check=True)
  time.sleep(3)
