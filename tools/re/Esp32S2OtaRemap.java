import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.MemoryBlock;

import java.io.ByteArrayInputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

public class Esp32S2OtaRemap extends GhidraScript {

	private static final int IMAGE_HEADER_SIZE = 0x18;
	private static final int SEGMENT_HEADER_SIZE = 8;
	private static final int ESP_IMAGE_MAGIC = 0xE9;

	private static class Segment {
		final int index;
		final long loadAddress;
		final byte[] data;
		final int fileOffset;

		Segment(int index, long loadAddress, byte[] data, int fileOffset) {
			this.index = index;
			this.loadAddress = loadAddress;
			this.data = data;
			this.fileOffset = fileOffset;
		}
	}

	private static long uint32(int value) {
		return value & 0xffffffffL;
	}

	private static String classify(long address) {
		if (address >= 0x3f000000L && address < 0x3ff80000L) {
			return "DROM";
		}
		if (address >= 0x3ff9e000L && address < 0x3ffa0000L) {
			return "RTC_DRAM";
		}
		if (address >= 0x3ffb0000L && address < 0x40000000L) {
			return "DRAM";
		}
		if (address >= 0x40020000L && address < 0x40070000L) {
			return "IRAM";
		}
		if (address >= 0x40070000L && address < 0x40072000L) {
			return "RTC_IRAM";
		}
		if (address >= 0x40080000L && address < 0x40800000L) {
			return "IROM";
		}
		if (address >= 0x50000000L && address < 0x50002000L) {
			return "RTC_DATA";
		}
		return "UNKNOWN";
	}

	private static boolean isExecutableType(String type) {
		return "IROM".equals(type) || "IRAM".equals(type) || "RTC_IRAM".equals(type);
	}

	private static boolean isWritableType(String type) {
		return "DRAM".equals(type) || "RTC_DRAM".equals(type) || "RTC_DATA".equals(type);
	}

	private static List<Segment> parseSegments(byte[] image) throws Exception {
		if (image.length < IMAGE_HEADER_SIZE) {
			throw new Exception("Image too small");
		}

		ByteBuffer header = ByteBuffer.wrap(image).order(ByteOrder.LITTLE_ENDIAN);
		int magic = header.get() & 0xff;
		if (magic != ESP_IMAGE_MAGIC) {
			throw new Exception(String.format("Unexpected image magic 0x%02x", magic));
		}

		int segmentCount = header.get() & 0xff;
		header.get(); // flash mode
		header.get(); // flash size/freq
		header.getInt(); // entry point is handled by the caller

		int offset = IMAGE_HEADER_SIZE;
		List<Segment> segments = new ArrayList<>();
		for (int i = 0; i < segmentCount; i++) {
			if (offset + SEGMENT_HEADER_SIZE > image.length) {
				throw new Exception("Segment header exceeds image size at index " + i);
			}
			ByteBuffer segHeader =
				ByteBuffer.wrap(image, offset, SEGMENT_HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN);
			long loadAddress = uint32(segHeader.getInt());
			int length = segHeader.getInt();
			int dataOffset = offset + SEGMENT_HEADER_SIZE;
			if (dataOffset + length > image.length) {
				throw new Exception("Segment data exceeds image size at index " + i);
			}
			byte[] data = new byte[length];
			System.arraycopy(image, dataOffset, data, 0, length);
			segments.add(new Segment(i, loadAddress, data, dataOffset));
			offset = dataOffset + length;
		}
		return segments;
	}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		String imagePath = args.length > 0 ? args[0] : currentProgram.getExecutablePath();
		byte[] image = Files.readAllBytes(Path.of(imagePath));

		ByteBuffer header = ByteBuffer.wrap(image).order(ByteOrder.LITTLE_ENDIAN);
		header.position(4);
		long entryPoint = uint32(header.getInt());
		List<Segment> segments = parseSegments(image);

		clearListing(currentProgram.getMemory(), true, true, true, true, true, true, true, true,
			true, true, true, true, true);

		List<MemoryBlock> existingBlocks = new ArrayList<>();
		for (MemoryBlock block : getMemoryBlocks()) {
			existingBlocks.add(block);
		}
		for (MemoryBlock block : existingBlocks) {
			removeMemoryBlock(block);
		}

		segments.sort(Comparator.comparingLong(seg -> seg.loadAddress));
		for (Segment seg : segments) {
			String type = classify(seg.loadAddress);
			String name = String.format("seg%d_%s", seg.index, type);
			Address start = toAddr(seg.loadAddress);
			MemoryBlock block =
				createMemoryBlock(name, start, new ByteArrayInputStream(seg.data), seg.data.length, false);
			block.setPermissions(true, isWritableType(type), isExecutableType(type));
			block.setComment(
				String.format("ESP32-S2 OTA segment %d fileOff=0x%08x type=%s", seg.index, seg.fileOffset, type));
			printf("Created block %s start=%s size=0x%x%n", name, start, seg.data.length);
		}

		Address entry = toAddr(entryPoint);
		addEntryPoint(entry);
		createLabel(entry, "entry", true);
		disassemble(entry);
		if (getFunctionAt(entry) == null) {
			createFunction(entry, "entry");
		}
		setPlateComment(entry, "ESP32-S2 OTA entry point");

		printf("Remapped OTA image and seeded entry point at %s%n", entry);
	}
}
