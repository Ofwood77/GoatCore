/* Synthetic S3B fixture, emitted and read by the official 26.1.1 RegionFile API.
 * Never part of mc_server. A fresh disposable directory and source chunk NBT
 * are mandatory. This is not naturally generated terrain. */
import java.nio.file.*;
import java.util.Arrays;
import java.util.Random;
import net.minecraft.nbt.*;
import net.minecraft.world.level.*;
import net.minecraft.world.level.chunk.storage.*;
public class write_external_fixture {
 public static void main(String[] args) throws Exception {
  net.minecraft.SharedConstants.tryDetectVersion(); net.minecraft.server.Bootstrap.bootStrap();
  Path dir=Path.of(args[0]);
  if(!Files.isRegularFile(dir.resolve(".goatcore-s3b-disposable")))throw new IllegalArgumentException("marker missing");
  CompoundTag root=NbtIo.read(Path.of(args[1]));
  int cx=root.getIntOr("xPos",0),cz=root.getIntOr("zPos",0);
  byte[] opaque=new byte[1200000];new Random(7754788L).nextBytes(opaque);root.putByteArray("S3B_external_synthetic",opaque);
  Path path=dir.resolve("r."+Math.floorDiv(cx,32)+"."+Math.floorDiv(cz,32)+".mca");
  if(Files.exists(path))throw new IllegalArgumentException("fresh fixture required");
  try(var region=new RegionFile(new RegionStorageInfo("s3b-disposable",Level.OVERWORLD,"chunk"),path,dir,true)) {
   try(var out=region.getChunkDataOutputStream(new ChunkPos(cx,cz))){NbtIo.write(root,out);}
   region.flush();
   try(var in=region.getChunkDataInputStream(new ChunkPos(cx,cz))){
    CompoundTag restored=NbtIo.read(in);
    if(!Arrays.equals(opaque,restored.getByteArray("S3B_external_synthetic").orElseThrow()))throw new AssertionError("external data mismatch");
   }
  }
 }
}
