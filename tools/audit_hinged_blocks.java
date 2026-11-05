/* Run only against the exact official 26.1.1 server classpath. No world/server. */
import net.minecraft.SharedConstants;
import net.minecraft.server.Bootstrap;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.block.*;
import net.minecraft.world.level.block.state.properties.BlockSetType;
public class audit_hinged_blocks {
 public static void main(String[] args) throws Exception {
  var out = new java.io.PrintStream(new java.io.FileOutputStream(java.io.FileDescriptor.out));
  SharedConstants.tryDetectVersion(); Bootstrap.bootStrap();
  var field=TrapDoorBlock.class.getDeclaredField("type");field.setAccessible(true);
  for(var block:BuiltInRegistries.BLOCK) {
   int kind=block instanceof DoorBlock?1:block instanceof TrapDoorBlock?2:0;
   if(kind!=0){
    BlockSetType type=kind==1?((DoorBlock)block).type():(BlockSetType)field.get(block);
    out.printf("F\t%s\t%d\t%d\t%d\t%d\t%d%n",BuiltInRegistries.BLOCK.getKey(block),kind,type.canOpenByHand()?1:0,
     BuiltInRegistries.ITEM.getId(block.asItem()),BuiltInRegistries.SOUND_EVENT.getId(kind==1?type.doorOpen():type.trapdoorOpen()),
     BuiltInRegistries.SOUND_EVENT.getId(kind==1?type.doorClose():type.trapdoorClose()));
   }
   for(var state:block.getStateDefinition().getPossibleStates()){
    int flags=(state.isCollisionShapeFullBlock(EmptyBlockGetter.INSTANCE,BlockPos.ZERO)?1:0)|
     (state.isFaceSturdy(EmptyBlockGetter.INSTANCE,BlockPos.ZERO,Direction.UP)?2:0)|
     (state.canBeReplaced()?4:0)|(state.isSignalSource()?8:0);
    out.printf("S\t%d\t%d%n",Block.getId(state),flags);
    if(kind!=0){
     var shape=state.getShape(EmptyBlockGetter.INSTANCE,BlockPos.ZERO).toAabbs();
     if(shape.size()!=1 || !shape.equals(state.getCollisionShape(EmptyBlockGetter.INSTANCE,BlockPos.ZERO).toAabbs()))
      throw new IllegalStateException("outline/collision mismatch: "+state);
     var b=shape.getFirst();
     out.printf(java.util.Locale.ROOT,"H\t%d\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f%n",Block.getId(state),b.minX*16,b.minY*16,b.minZ*16,b.maxX*16,b.maxY*16,b.maxZ*16);
    }
   }
  }
 }
}
