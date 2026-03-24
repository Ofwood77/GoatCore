Expected files:

1) registry_packets_1_21_1.bin
   - Binary file containing a sequence of packets.
   - Format: [VarInt length][payload bytes] repeated until EOF.
   - Each payload is a Configuration Clientbound "Registry Data" packet payload
     (do NOT include the packet ID or outer length prefix).

2) chunk_0_0.bin
   - Binary file containing a full payload for Clientbound "Chunk Data and Update Light".
   - The payload must match chunk X/Z = (0,0) and the format for protocol 1.21.1.
