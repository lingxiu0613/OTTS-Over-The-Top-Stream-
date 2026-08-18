function readBe24(buffer, offset) {
  return (buffer[offset] << 16) | (buffer[offset + 1] << 8) | buffer[offset + 2];
}

export function createFlvTagParser(onTag) {
  let buffer = Buffer.alloc(0);
  let headerSkipped = false;

  return {
    push(chunk) {
      if (!chunk || !chunk.length) {
        return;
      }
      buffer = Buffer.concat([buffer, chunk]);

      if (!headerSkipped) {
        if (buffer.length < 13) {
          return;
        }
        if (buffer.subarray(0, 3).toString("ascii") !== "FLV") {
          throw new Error("invalid flv header from ffmpeg");
        }
        buffer = buffer.subarray(13);
        headerSkipped = true;
      }

      while (buffer.length >= 15) {
        const tagType = buffer[0];
        const dataSize = readBe24(buffer, 1);
        const timestamp = readBe24(buffer, 4) | (buffer[7] << 24);
        const totalSize = 11 + dataSize + 4;
        if (buffer.length < totalSize) {
          return;
        }
        const payload = buffer.subarray(11, 11 + dataSize);
        onTag({
          typeId: tagType,
          timestamp,
          messageStreamId: 1,
          payload: Buffer.from(payload)
        });
        buffer = buffer.subarray(totalSize);
      }
    }
  };
}
