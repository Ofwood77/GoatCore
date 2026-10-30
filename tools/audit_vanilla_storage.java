import net.minecraft.SharedConstants;
import net.minecraft.server.Bootstrap;
import net.minecraft.core.registries.BuiltInRegistries;
public class audit_vanilla_storage {
 public static void main(String[] args) {
  var out = new java.io.PrintStream(new java.io.FileOutputStream(java.io.FileDescriptor.out));
  SharedConstants.tryDetectVersion(); Bootstrap.bootStrap();
  for(var type:BuiltInRegistries.BLOCK_ENTITY_TYPE)for(var block:BuiltInRegistries.BLOCK)
   if(type.isValid(block.defaultBlockState()))out.println("PAIR\t"+BuiltInRegistries.BLOCK_ENTITY_TYPE.getKey(type)+"\t"+BuiltInRegistries.BLOCK.getKey(block));
  for(var block:BuiltInRegistries.BLOCK) if(block.getStateDefinition().getPossibleStates().stream().anyMatch(net.minecraft.world.entity.ai.village.poi.PoiTypes::hasPoi))out.println("POI\t"+BuiltInRegistries.BLOCK.getKey(block));
  for(var status:BuiltInRegistries.CHUNK_STATUS)out.println("STATUS\t"+BuiltInRegistries.CHUNK_STATUS.getKey(status));
 }
}
