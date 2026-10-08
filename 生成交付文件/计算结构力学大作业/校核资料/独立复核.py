# 独立复核工具：需要 Python 3 和 numpy；不是 C 程序运行依赖。
# 分别验证 numpy 矩阵解、解析手算公式和 C 输出，不修改交付结果。
from pathlib import Path
import tempfile, subprocess, numpy as np, json, os
out=Path(__file__).resolve().parents[1]
exe=out/'程序'/('truss.exe' if os.name=='nt' else 'truss_linux')
tmp=tempfile.TemporaryDirectory()
results={}
for label in ['必做题','选做题']:
 inp=out/f'总控信息/{label}_总控信息.txt'; output=Path(tmp.name)/f'{label}_机算结果.txt'
 subprocess.run([str(exe),str(inp),str(output)],check=True)
 # Independent dense assembly and numpy general LU solve, not C Cholesky.
 lines=[l.split('#',1)[0].strip() for l in inp.read_text().splitlines()]; lines=[l for l in lines if l]
 n=int(lines[1].split()[1]); nodes=np.array([[float(v) for v in l.split()] for l in lines[2:2+n]])
 m=int(lines[2+n].split()[1]); elems=np.array([[float(v) for v in l.split()] for l in lines[3+n:3+n+m]])
 K=np.zeros((2*n,2*n)); B=np.zeros((m,2*n)); lengths=[]; ids={int(r[0]):i for i,r in enumerate(nodes)}
 for e,row in enumerate(elems):
  i,j=ids[int(row[1])],ids[int(row[2])]; delta=nodes[j,1:3]-nodes[i,1:3]; L=np.linalg.norm(delta); c,s=delta/L
  B[e,[2*i,2*i+1,2*j,2*j+1]]=[-c,-s,c,s]; K+=row[3]*row[4]/L*np.outer(B[e],B[e]); lengths.append(L)
 free=np.flatnonzero(nodes[:,3:5].ravel()==0); F=nodes[:,7:9].ravel(); u=nodes[:,5:7].ravel().copy(); u[free]=np.linalg.solve(K[np.ix_(free,free)],F[free]); N=elems[:,3]*elems[:,4]/np.array(lengths)*(B@u); R=K@u-F
 text=output.read_text(); nr=text.split('NODE RESULTS\n')[1].split('\n\n')[0].splitlines()[1:]; er=text.split('ELEMENT RESULTS\n')[1].split('\n\n')[0].splitlines()[1:]
 cu=np.array([[float(x) for x in r.split()[1:3]] for r in nr]).ravel(); cn=np.array([float(r.split()[4]) for r in er]); np.testing.assert_allclose(cu,u,rtol=1e-10,atol=1e-14); np.testing.assert_allclose(cn,N,rtol=1e-10,atol=1e-10)
 # Analytical/manual result via algebraic equations.
 if label=='必做题':
  EA=4.8e6; l=h=1.; a=l/2;b=l/(2*np.sqrt(3)); d=h-b;L=np.hypot(a,h);c=a/L;s=h/L;ko=EA/L;ki=EA/(l/np.sqrt(3));kv=EA/d;Px=20+100/np.sqrt(2);Py=-100/np.sqrt(2);D=(ki/2+kv)*(2*ko*s*s+kv)-kv*kv
  um=np.array([0,0,0,0,0,kv*Py/D,Px/(2*ko*c*c),(ki/2+kv)*Py/D]);nm=np.array([ko*(c*um[6]+s*um[7]),kv*(um[7]-um[5]),ko*(-c*um[6]+s*um[7]),ki*um[5]/2,ki*um[5]/2,0])
 else:
  R3=(30*2+50*6+15*1.5)/8;R1=80-R3; nm=np.zeros(7);nm[2]=-R1/.6;nm[0]=15-.8*nm[2];nm[5]=-R3/.6;nm[1]=-.8*nm[5];nm[3]=-50-nm[2];nm[4]=-nm[3];nm[6]=.8*(nm[2]-nm[3]); dl=nm*np.array(lengths)/200000
  u2x=dl[0];u3x=u2x+dl[1];u4x=(dl[4]-1.6*dl[6]+1.6*u2x-dl[5]+.8*u3x+dl[2]-dl[3])/3.2;u4y=(dl[2]-.8*u4x)/.6;u2y=(.8*(u2x-u4x)-dl[3])/.6+u4y;u5x=u4x+dl[6];u5y=(dl[5]-.8*(u3x-u5x))/.6
  um=np.array([0,0,u2x,u2y,u3x,0,u4x,u4y,u5x,u5y]); np.testing.assert_allclose(.8*(u5x-u2x)+.6*(u5y-u2y),dl[4],atol=1e-14)
 print('manual',label,nm,um); np.testing.assert_allclose(um,u,rtol=1e-10,atol=1e-14);np.testing.assert_allclose(nm,N,rtol=1e-10,atol=1e-10)
 results[label]={'nodes':nodes.tolist(),'elements':elems.tolist(),'Kff':K[np.ix_(free,free)].tolist(),'free':free.tolist(),'u':u.tolist(),'N':N.tolist(),'R':R.tolist(),'L':lengths,'manual_u':um.tolist(),'manual_N':nm.tolist(),'extension':(N*np.array(lengths)/(elems[:,3]*elems[:,4])).tolist(),'max_u_error':float(max(abs(cu-um))),'max_N_error':float(max(abs(cn-nm))),'checks':text.split('CHECKS\n')[1]}
 print(label,'u=',u,'N=',N,'R=',R)

print('两题全部节点位移和全部杆件轴力复核通过。')
tmp.cleanup()
