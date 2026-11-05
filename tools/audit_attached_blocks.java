/* Exact 26.1.1 classes and builtin data tags; no server or world is started. */
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import com.google.gson.*;
import net.minecraft.SharedConstants;
import net.minecraft.server.Bootstrap;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.block.*;
import net.minecraft.world.entity.decoration.ArmorStand;
public class audit_attached_blocks {
 static Set<String> tag(String name,int depth) throws Exception {
  if(depth>32)throw new IllegalStateException("tag cycle");
  String[] n=name.split(":",2);String path="data/"+n[0]+"/tags/block/"+n[1]+".json";
  try(var input=audit_attached_blocks.class.getClassLoader().getResourceAsStream(path)){
   if(input==null)throw new FileNotFoundException(path);
   Set<String> result=new TreeSet<>();
   for(var v:JsonParser.parseReader(new InputStreamReader(input,StandardCharsets.UTF_8)).getAsJsonObject().getAsJsonArray("values")){
    String id=v.isJsonObject()?v.getAsJsonObject().get("id").getAsString():v.getAsString();
    if(id.startsWith("#"))result.addAll(tag(id.substring(1),depth+1));else result.add(id);
   }return result;
  }
 }
 public static void main(String[] args) throws Exception {
  var out=new PrintStream(new FileOutputStream(FileDescriptor.out));
  SharedConstants.tryDetectVersion();Bootstrap.bootStrap();
  Set<String> unstable=tag("minecraft:unstable_bottom_center",0);
  for(String name:unstable)out.printf("U\t%s%n",name);
  var order=net.minecraft.world.level.block.state.BlockBehaviour.class.getDeclaredField("UPDATE_SHAPE_ORDER");order.setAccessible(true);
  out.print("N");for(Direction d:(Direction[])order.get(null))out.print("\t"+d.get3DDataValue());out.println();
  for(var block:BuiltInRegistries.BLOCK){
   String name=BuiltInRegistries.BLOCK.getKey(block).toString();
   int kind=block instanceof LanternBlock?3:block instanceof LadderBlock?4:
    block instanceof WallTorchBlock && !(block instanceof RedstoneWallTorchBlock)?2:
    block instanceof TorchBlock && !(block instanceof RedstoneTorchBlock)?1:0;
   if(kind!=0)out.printf("F\t%s\t%d\t%d\t%d%n",name,kind,BuiltInRegistries.ITEM.getId(block.asItem()),Block.getId(block.defaultBlockState()));
   for(var state:block.getStateDefinition().getPossibleStates()){
    int mask=0;
    for(Direction d:Direction.values()){
     if(state.isFaceSturdy(EmptyBlockGetter.INSTANCE,BlockPos.ZERO,d,SupportType.FULL))mask|=1<<d.get3DDataValue();
     if(!(d==Direction.DOWN&&unstable.contains(name))&&state.isFaceSturdy(EmptyBlockGetter.INSTANCE,BlockPos.ZERO,d,SupportType.CENTER))mask|=1<<(6+d.get3DDataValue());
     if(state.isFaceSturdy(EmptyBlockGetter.INSTANCE,BlockPos.ZERO,d,SupportType.RIGID))mask|=1<<(12+d.get3DDataValue());
    }
    out.printf("S\t%d\t%d%n",Block.getId(state),mask);
    if(kind==0)continue;
    out.printf("L\t%d\t%d%n",Block.getId(state),state.getLightEmission());
    float speed=state.getDestroySpeed(EmptyBlockGetter.INSTANCE,BlockPos.ZERO);
    out.printf("B\t%d\t%d\t%d%n",Block.getId(state),Math.round(speed*100),state.requiresCorrectToolForDrops()?1:0);
    var all=List.of(state.getShape(EmptyBlockGetter.INSTANCE,BlockPos.ZERO),state.getCollisionShape(EmptyBlockGetter.INSTANCE,BlockPos.ZERO));
    for(int type=0;type<2;type++){
     var boxes=all.get(type).toAabbs();out.printf("H\t%d\t%d\t%d",Block.getId(state),type,boxes.size());
     for(var box:boxes){for(double v:new double[]{box.minX,box.minY,box.minZ,box.maxX,box.maxY,box.maxZ})if(Math.rint(v*32)!=v*32)throw new IllegalStateException("fractional shape unit");}
     for(var box:boxes)out.printf(Locale.ROOT,"\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f",box.minX*32,box.minY*32,box.minZ*32,box.maxX*32,box.maxY*32,box.maxZ*32);
     out.println();
    }
   }
  }
  var sf=net.minecraft.util.Mth.class.getDeclaredField("SIN");sf.setAccessible(true);float[] sin=(float[])sf.get(null);
  for(int i=0;i<sin.length;i++)out.printf("T\t%d\t%08x%n",i,Float.floatToRawIntBits(sin[i]));
  var uf=sun.misc.Unsafe.class.getDeclaredField("theUnsafe");uf.setAccessible(true);var unsafe=(sun.misc.Unsafe)uf.get(null);
  var entity=(ArmorStand)unsafe.allocateInstance(ArmorStand.class);
  for(float yaw:new float[]{-180,-135.01f,-135,-134.99f,-90,-45.01f,-45,-44.99f,0,44.99f,45,45.01f,90,134.99f,135,135.01f,180,270,360})
   for(float pitch:new float[]{-90,-60,-45.01f,-45,-44.99f,-30,0,30,44.99f,45,45.01f,60,90}){
    entity.setXRot(pitch);entity.setYRot(yaw);entity.setYHeadRot(yaw);out.printf(Locale.ROOT,"O\t%.8g\t%.8g",yaw,pitch);
    for(var d:Direction.orderedByNearest(entity))out.print("\t"+d.get3DDataValue());out.println();
   }
 }
}
