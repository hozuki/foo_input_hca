#include "stdafx.h"

// Note: the skeleton is from 'foo_sample', apparently.

enum {
	hca_bits_per_sample = 16,
	hca_channels = 2,
	hca_sample_rate = 44100,

	hca_bytes_per_sample = hca_bits_per_sample / 8,
	hca_total_sample_width = hca_bytes_per_sample * hca_channels,
};

#define CGSS_KEY_1 (0xf27e3b22)
#define CGSS_KEY_2 (0x00003657)

#define DEBUG_PLUGIN 0

class input_hca {

public:
	void open(service_ptr_t<file> p_filehint, const char * p_path, t_input_open_reason p_reason, abort_callback & p_abort) {
		if (p_reason == input_open_info_write) {
			throw exception_io_unsupported_format();//our input does not support retagging.
		}
		m_file = p_filehint;//p_filehint may be null, hence next line
		input_open_file_helper(m_file, p_path, p_reason, p_abort);//if m_file is null, opens file with appropriate privileges for our operation (read/write for writing tags, read-only otherwise).
#if DEBUG_PLUGIN
		fp = fopen("hcadec.log", "w");
#endif
	}

	void get_info(file_info & p_info, abort_callback & p_abort) {
		if (m_info_retrieved) {
			write_log("get_info called: from cache\n");
			auto length_in_samples = m_hca_info.blockCount * m_hca_info.channelCount * 0x80 * 8;
			p_info.set_length(audio_math::samples_to_time(length_in_samples / m_hca_info.channelCount, m_hca_info.samplingRate));
			p_info.info_set_int("samplerate", m_hca_info.samplingRate);
			p_info.info_set_int("channels", m_hca_info.channelCount);
			p_info.info_set_int("bitspersample", hca_bits_per_sample);
			p_info.info_set("encoding", "Lossy");
			p_info.info_set_bitrate((hca_bits_per_sample * hca_channels * hca_sample_rate + 500 /* rounding for bps to kbps*/) / 1000 /* bps to kbps */);
		} else {
			t_filesize size = m_file->get_size(p_abort);
			//note that the file size is not always known, for an example, live streams and alike have no defined size and filesize_invalid is returned
			if (size != filesize_invalid) {
				write_log("get_info called: new\n");
				pfc::array_t<t_uint8> buffer;
				buffer.set_size(size);
				write_log("#1");
				auto read_size = m_file->read(buffer.get_ptr(), size, p_abort);
				write_log("#2");
				KS_DECODE_HANDLE decode;
				KsOpenBuffer(buffer.get_ptr(), size, FALSE, &decode);
				write_log("#3");
				KsBeginDecode(decode);
				write_log("#4");
				KsGetHcaInfo(decode, &m_hca_info);
				write_log("#5");
				KsEndDecode(decode);
				write_log("#6");
				KsCloseHandle(decode);
				write_log("#7");
				decode = nullptr;

				auto length_in_samples = m_hca_info.blockCount * m_hca_info.channelCount * 0x80 * 8;
				auto length_in_seconds = (double)length_in_samples / m_hca_info.channelCount / m_hca_info.samplingRate;
				write_log("#7.5");
				//file size is known, let's set length
				//p_info.set_length(audio_math::samples_to_time(length_in_samples / m_hca_info.channelCount, m_hca_info.samplingRate));
				p_info.set_length(length_in_seconds);
				write_log("#8");
				p_info.info_set_int("samplerate", m_hca_info.samplingRate);
				write_log("#9");
				p_info.info_set_int("channels", m_hca_info.channelCount);
				write_log("#10");
				p_info.info_set_int("bitspersample", hca_bits_per_sample);
				write_log("#11");
				p_info.info_set("encoding", "Lossy");
				write_log("#12");
				p_info.info_set_bitrate((hca_bits_per_sample * hca_channels * hca_sample_rate + 500 /* rounding for bps to kbps*/) / 1000 /* bps to kbps */);
				write_log("#13");
				m_info_retrieved = true;
			} else {
				write_log("get_info called: file invalid\n");
				//note that the values below should be based on contents of the file itself, NOT on user-configurable variables for an example. To report info that changes independently from file contents, use get_dynamic_info/get_dynamic_info_track instead.
				p_info.info_set_int("samplerate", hca_sample_rate);
				p_info.info_set_int("channels", hca_channels);
				p_info.info_set_int("bitspersample", hca_bits_per_sample);
				p_info.info_set("encoding", "Lossy");
				p_info.info_set_bitrate((hca_bits_per_sample * hca_channels * hca_sample_rate + 500 /* rounding for bps to kbps*/) / 1000 /* bps to kbps */);
			}
		}
	}

	t_filestats get_file_stats(abort_callback & p_abort) const {
		return m_file->get_stats(p_abort);
	}

	void decode_initialize(unsigned p_flags, abort_callback & p_abort) {
		write_log("Decode started.\n");
		m_file->reopen(p_abort);//equivalent to seek to zero, except it also works on nonseekable streams
		m_buffer.set_size(m_file->get_size(p_abort));
		m_file->read(m_buffer.get_ptr(), m_buffer.get_size(), p_abort);
		KsOpenBuffer(m_buffer.get_ptr(), m_buffer.get_size(), FALSE, &m_decode);
		KsSetParamI32(m_decode, KS_PARAM_KEY1, CGSS_KEY_1);
		KsSetParamI32(m_decode, KS_PARAM_KEY2, CGSS_KEY_2);
		KsBeginDecode(m_decode);
		uint32 buffer_size;
		KsGetWaveHeader(m_decode, nullptr, &buffer_size); // dummy
		uint8 *b = new uint8[buffer_size];
		KsGetWaveHeader(m_decode, b, &buffer_size);
		delete[] b;
		KsDecodeData(m_decode, nullptr, &m_data_buffer_size);
	}

	bool decode_run(audio_chunk & p_chunk, abort_callback & p_abort) {
		pfc::array_t<t_uint8> buffer;
		const uint32 r = 10;
		auto read_size = m_data_buffer_size * r;
		buffer.set_size(read_size);
		KS_RESULT result = KsDecodeData(m_decode, buffer.get_ptr(), &read_size);
		if (result <= 0 || read_size <= 0) {
			cleanup_hca();
			return false;
		}
		p_chunk.set_data_fixedpoint(buffer.get_ptr(), read_size, hca_sample_rate, hca_channels, hca_bits_per_sample, audio_chunk::g_guess_channel_config(hca_channels));

		if (read_size < m_data_buffer_size * r) {
			cleanup_hca();
			return false;
		}

		//processed successfully, no EOF
		return true;
	}

	void decode_seek(double p_seconds, abort_callback & p_abort) {
		throw exception_io_object_not_seekable("kawashima does not support seeking right now.");

		m_file->ensure_seekable();//throw exceptions if someone called decode_seek() despite of our input having reported itself as nonseekable.
								  // IMPORTANT: convert time to sample offset with proper rounding! audio_math::time_to_samples does this properly for you.
		t_filesize target = audio_math::time_to_samples(p_seconds, hca_sample_rate) * hca_total_sample_width;

		// get_size_ex fails (throws exceptions) if size is not known (where get_size would return filesize_invalid). Should never fail on seekable streams (if it does it's not our problem anymore).
		t_filesize max = m_file->get_size_ex(p_abort);
		if (target > max) target = max;//clip seek-past-eof attempts to legal range (next decode_run() call will just signal EOF).

		m_file->seek(target, p_abort);
	}

	bool decode_can_seek() const {
		// kawashima does not support seeking right now
		return false;
	}

	bool decode_get_dynamic_info(file_info & p_out, double & p_timestamp_delta) const {
		// deals with dynamic information such as VBR bitrates
		return false;
	}

	bool decode_get_dynamic_info_track(file_info & p_out, double & p_timestamp_delta) const {
		// deals with dynamic information such as track changes in live streams
		return false;
	}

	void decode_on_idle(abort_callback & p_abort) const {
		m_file->on_idle(p_abort);
	}

	void retag(const file_info & p_info, abort_callback & p_abort) const {
		throw exception_io_unsupported_format();
	}

	static bool g_is_our_content_type(const char * p_content_type) {
		// match against supported mime types here
		return false;
	}

	static bool g_is_our_path(const char * p_path, const char * p_extension) {
		return stricmp_utf8(p_extension, "hca") == 0;
	}

private:
	service_ptr_t<file> m_file;
	pfc::array_t<t_uint8> m_buffer;
	KS_DECODE_HANDLE m_decode;
	uint32 m_data_buffer_size;
	HCA_INFO m_hca_info;
	bool m_info_retrieved = false;
#if DEBUG_PLUGIN
	FILE *fp = nullptr;
#endif

	void write_log(const char *format, ...) {
#if DEBUG_PLUGIN
		if (!fp) {
			return;
		}
		va_list ap;
		va_start(ap, format);
		vfprintf(fp, format, ap);
		va_end(ap);
		fflush(fp);
#endif
	}

	void cleanup_hca() {
#if DEBUG_PLUGIN
		fclose(fp);
		fp = nullptr;
#endif
		if (m_decode && KsIsActiveHandle(m_decode)) {
			KsEndDecode(m_decode);
			KsCloseHandle(m_decode);
		}
		m_decode = nullptr;
	}

};

static input_singletrack_factory_t<input_hca> g_input_hca_factory;
DECLARE_FILE_TYPE("CRI HCA Audio", "*.hca")
