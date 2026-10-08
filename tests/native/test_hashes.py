"""Independent SHA-256 vectors and PNG identity fixtures. Development only."""
import ctypes,hashlib,random,struct,unittest,zlib
from pathlib import Path
LIB=ctypes.CDLL(str(Path(__file__).resolve().parents[2]/'build/libidentity.so'))
LIB.png_pixel_hash.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p]
LIB.sha256.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p]

def chunk(kind,data): return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
def digest(data):
 out=ctypes.create_string_buffer(32)
 valid=LIB.png_pixel_hash(data,len(data),out)
 return out.raw if valid else None

def expected(width,height,pixels):
 return hashlib.sha256(b'CQ-PNG-RGBA16'+struct.pack('>II',width,height)+b''.join(struct.pack('>4H',*p) for p in pixels)).digest()

def png(width,height,color,depth,raw,extras=b'',interlace=0,level=6):
 return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,depth,color,0,0,interlace))+extras+chunk(b'IDAT',zlib.compress(raw,level))+chunk(b'IEND',b'')

def pack_samples(samples,depth):
 if depth==16:return b''.join(struct.pack('>H',v) for v in samples)
 if depth==8:return bytes(samples)
 bits=''.join(format(v,f'0{depth}b') for v in samples)
 bits+='0'*((-len(bits))%8)
 return bytes(int(bits[i:i+8],2) for i in range(0,len(bits),8))

class IdentityTests(unittest.TestCase):
 def test_sha_standard_vectors(self):
  for data in [b'',b'abc',b'a'*1000000,bytes(range(256))*31]:
   out=ctypes.create_string_buffer(32);LIB.sha256(data,len(data),out)
   self.assertEqual(out.raw,hashlib.sha256(data).digest())

 def test_all_png_filters(self):
  rows=[bytes([10,20,30,40,50,60,70,80,90]),bytes([80,70,60,50,40,30,20,10,0])]
  pixels=[tuple(v*257 for v in row[x:x+3])+(65535,) for row in rows for x in range(0,len(row),3)]
  def paeth(a,b,c):
   p=a+b-c
   return min((a,b,c),key=lambda v:abs(p-v))
  for filter in range(5):
   raw=b''
   for y,row in enumerate(rows):
    encoded=[]
    for i,v in enumerate(row):
     a=row[i-3] if i>=3 else 0;b=rows[y-1][i] if y else 0;c=rows[y-1][i-3] if y and i>=3 else 0
     pred=[0,a,b,(a+b)//2,paeth(a,b,c)][filter];encoded.append((v-pred)&255)
    raw+=bytes([filter])+bytes(encoded)
   self.assertEqual(digest(png(3,2,2,8,raw)),expected(3,2,pixels))

 def test_metadata_compression_rgb_rgba_equivalence(self):
  a=png(2,1,2,8,b'\0\x10\x20\x30\x40\x50\x60')
  b=png(2,1,6,8,b'\0\x10\x20\x30\xff\x40\x50\x60\xff',chunk(b'tEXt',b'Comment\0different metadata'),level=9)
  self.assertEqual(digest(a),digest(b));self.assertIsNotNone(digest(a))
  changed=png(2,1,2,8,b'\0\x11\x20\x30\x40\x50\x60')
  self.assertNotEqual(digest(a),digest(changed))

 def test_gray_palette_and_transparency(self):
  for depth in [1,2,4,8,16]:
   maximum=(1<<depth)-1;values=[0,maximum,maximum//2]
   raw=b'\0'+pack_samples(values,depth)
   pixels=[(v*65535//maximum,)*3+(0 if v==maximum else 65535,) for v in values]
   self.assertEqual(digest(png(3,1,0,depth,raw,chunk(b'tRNS',struct.pack('>H',maximum)))),expected(3,1,pixels))
  palette=bytes([10,20,30,40,50,60])
  extras=chunk(b'PLTE',palette)+chunk(b'tRNS',bytes([255,80]))
  self.assertEqual(digest(png(3,1,3,1,b'\0'+pack_samples([0,1,0],1),extras)),expected(3,1,[(2570,5140,7710,65535),(10280,12850,15420,20560),(2570,5140,7710,65535)]))
  self.assertEqual(digest(png(1,1,4,8,b'\0\x33\x80')),expected(1,1,[(0x3333,0x3333,0x3333,0x8080)]))
  self.assertEqual(digest(png(1,1,6,16,b'\0'+struct.pack('>4H',1,257,32000,65535))),expected(1,1,[(1,257,32000,65535)]))

 def test_adam7_interlace(self):
  w,h=11,9
  pixels=[(x*17%256,y*23%256,(x+y)*13%256,255) for y in range(h) for x in range(w)]
  raw=b''
  for x0,y0,dx,dy in [(0,0,8,8),(4,0,8,8),(0,4,4,8),(2,0,4,4),(0,2,2,4),(1,0,2,2),(0,1,1,2)]:
   for y in range(y0,h,dy):
    row=b''.join(bytes(pixels[y*w+x]) for x in range(x0,w,dx))
    if row:raw+=b'\0'+row
  self.assertEqual(digest(png(w,h,6,8,raw,interlace=1)),expected(w,h,[tuple(v*257 for v in p) for p in pixels]))

 def test_invalid_animated_and_oversize_fallback(self):
  good=png(1,1,2,8,b'\0\0\0\0')
  for n in range(len(good)):
   self.assertIsNone(digest(good[:n]))
  damaged=bytearray(good);damaged[-1]^=1;self.assertIsNone(digest(bytes(damaged)))
  self.assertIsNone(digest(png(100000,100000,2,8,b'')))
  self.assertIsNone(digest(png(1,1,2,8,b'\x05\0\0\0')))
  self.assertIsNone(digest(png(1,1,2,8,b'\0\0\0\0',chunk(b'acTL',struct.pack('>II',2,0)))))
  # Deterministic malformed-header corpus, including valid CRCs.
  rng=random.Random(128)
  for _ in range(400):
   raw=rng.randbytes(rng.randrange(0,300))
   self.assertIsNone(digest(raw))
   payload=png(rng.randrange(1,40),rng.randrange(1,40),rng.randrange(0,8),rng.choice([1,2,4,8,16]),raw)
   digest(payload)

if __name__=='__main__': unittest.main()
