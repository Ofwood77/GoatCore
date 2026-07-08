Expected files:

1) registry_packets_26_1_1.bin
   - Binary file containing a sequence of packets.
   - Format: [VarInt length][payload bytes] repeated until EOF.
   - Each payload is a Configuration Clientbound "Registry Data" packet payload
     (do NOT include the packet ID or outer length prefix).

2) chunk_0_0_26_1_1.bin
   - Binary file containing a full payload for Clientbound "Chunk Data and Update Light".
   - The payload must match chunk X/Z = (0,0) and the format for Minecraft 26.1.1 / protocol 775.

3) tags_packet_26_1_1.bin
   - Binary payload for the Configuration Clientbound "Update Tags" packet.
