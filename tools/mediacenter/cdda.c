// ---------------------------------------------------------------------------
// Audio CD (included by mediacenter.c)
//
// The audio tracks of the disc in the drive (or of a .bin/.cue image) play as
// one stream with no pause between tracks: FFmpeg reads a WAV file made up
// here (a 44-byte header, then the 2352-byte sectors as they are, 44.1 kHz
// 16-bit stereo), so pause, seek, fast forward and the audio screen work as
// for a file. The tracks are the chapters: right/left change track, and the
// screen shows the track number, its time and its name (CD-Text on the
// disc, TITLE/PERFORMER lines in a .cue).
//
// A thread reads the sectors ahead (up to CD_AHEAD seconds) at a low drive
// speed (MC_CD_SPEED, 8x by default: quieter, and time for re-reads). A
// sector that cannot be read after the re-reads plays as silence.
// ---------------------------------------------------------------------------

#define CD_RATE 44100
#define CD_AHEAD 10 // seconds read ahead
#define CD_CAP (CD_AHEAD * 75)
#define CD_CHUNK 16 // sectors per read (0.21 s)
#define CD_HDR 44

static struct
{
	int first, last;          // audio tracks played
	int start, end;           // disc sectors of the stream
	uint8_t *ring;            // CD_CAP sectors, slot = lba % CD_CAP
	int lo, hi;               // sectors held: lo..hi-1
	int cur;                  // sector being read by FFmpeg
	unsigned gen;             // changes when the reading jumps (seek)
	int running;
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int64_t pos;              // position in the WAV file
	uint8_t hdr[CD_HDR];
	int bad, reread;          // sectors left silent, sectors read again
	int shown;                // track shown on the screen (-1 none yet)
	char album[128], artist[128];
	char title[MAX_TRACKS + 1][128], performer[MAX_TRACKS + 1][128];
} cd = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

// READ CD of CD-DA sectors (expected type 1, user data only: 2352 bytes).
static int cd_read_sg(int lba, int count, uint8_t *dst)
{
	uint8_t cdb[12] = { 0xBE, 0x04 };
	uint8_t sense[32];
	sg_io_hdr_t io;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[6] = (count >> 16) & 0xFF;
	cdb[7] = (count >> 8) & 0xFF;
	cdb[8] = count & 0xFF;
	cdb[9] = 0x10;
	memset(&io, 0, sizeof(io));
	io.interface_id = 'S';
	io.cmd_len = sizeof(cdb);
	io.cmdp = cdb;
	io.dxfer_direction = SG_DXFER_FROM_DEV;
	io.dxferp = dst;
	io.dxfer_len = count * RAW_SECTOR;
	io.sbp = sense;
	io.mx_sb_len = sizeof(sense);
	io.timeout = 10000;
	if (ioctl(cd_fd, SG_IO, &io) < 0) return -1;
	if ((io.info & SG_INFO_OK_MASK) != SG_INFO_OK) return -1;
	return 0;
}

// The kernel's own audio read (for drives or bridges without SG_IO).
static int cd_read_ioctl(int lba, int count, uint8_t *dst)
{
	struct cdrom_read_audio ra;
	memset(&ra, 0, sizeof(ra));
	ra.addr.lba = lba;
	ra.addr_format = CDROM_LBA;
	ra.nframes = count;
	ra.buf = dst;
	return ioctl(cd_fd, CDROMREADAUDIO, &ra) ? -1 : 0;
}

static void cd_read(int lba, int count, uint8_t *dst)
{
	if (img_fd >= 0)
	{
		ssize_t len = (ssize_t)count * RAW_SECTOR;
		ssize_t n = pread(img_fd, dst, len, (off_t)lba * RAW_SECTOR);
		if (n < 0) n = 0;
		if (n < len) memset(dst + n, 0, len - n);
		return;
	}
	for (int a = 0; a < 3 && cd.running; a++)
	{
		if (!cd_read_sg(lba, count, dst) || !cd_read_ioctl(lba, count, dst))
		{
			if (a) cd.reread += count;
			return;
		}
	}
	// one sector at a time: only the bad ones go silent
	for (int i = 0; i < count; i++)
	{
		uint8_t *d = dst + (size_t)i * RAW_SECTOR;
		int ok = 0;
		for (int a = 0; a < 3 && !ok && cd.running; a++) ok = !cd_read_sg(lba + i, 1, d) || !cd_read_ioctl(lba + i, 1, d);
		if (!ok)
		{
			memset(d, 0, RAW_SECTOR);
			cd.bad++;
		}
		else cd.reread++;
	}
}

static void *cd_reader(void *arg)
{
	(void)arg;
	uint8_t *buf = (uint8_t *)malloc((size_t)CD_CHUNK * RAW_SECTOR);
	if (!buf) return NULL;
	pthread_mutex_lock(&cd.lock);
	while (cd.running)
	{
		if (cd.hi >= cd.end || cd.hi - cd.cur >= CD_CAP - CD_CHUNK)
		{
			pthread_cond_wait(&cd.cond, &cd.lock);
			continue;
		}
		int lba = cd.hi, n = cd.end - lba;
		if (n > CD_CHUNK) n = CD_CHUNK;
		unsigned gen = cd.gen;
		pthread_mutex_unlock(&cd.lock);
		cd_read(lba, n, buf);
		pthread_mutex_lock(&cd.lock);
		if (gen != cd.gen || lba != cd.hi) continue; // a seek meanwhile
		for (int i = 0; i < n; i++)
			memcpy(cd.ring + (size_t)((lba + i) % CD_CAP) * RAW_SECTOR, buf + (size_t)i * RAW_SECTOR, RAW_SECTOR);
		cd.hi += n;
		if (cd.lo < cd.hi - CD_CAP) cd.lo = cd.hi - CD_CAP;
		pthread_cond_broadcast(&cd.cond);
	}
	pthread_mutex_unlock(&cd.lock);
	free(buf);
	return NULL;
}

static int cd_io_read(void *opaque, uint8_t *buf, int size)
{
	(void)opaque;
	int64_t total = CD_HDR + (int64_t)(cd.end - cd.start) * RAW_SECTOR;
	if (cd.pos >= total || quit_requested) return AVERROR_EOF;
	int done = 0;
	if (cd.pos < CD_HDR)
	{
		int n = CD_HDR - (int)cd.pos;
		if (n > size) n = size;
		memcpy(buf, cd.hdr + cd.pos, n);
		cd.pos += n;
		return n;
	}
	int64_t off = cd.pos - CD_HDR;
	int lba = cd.start + (int)(off / RAW_SECTOR), in = (int)(off % RAW_SECTOR);
	pthread_mutex_lock(&cd.lock);
	if (lba < cd.lo || lba > cd.hi)
	{
		// outside what is held: read from there
		cd.lo = cd.hi = lba;
		cd.gen++;
	}
	cd.cur = lba;
	pthread_cond_broadcast(&cd.cond);
	while (cd.hi <= lba && cd.running && !quit_requested)
	{
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 100000000;
		if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
		pthread_cond_timedwait(&cd.cond, &cd.lock, &ts);
	}
	while (done < size && lba < cd.hi)
	{
		int n = RAW_SECTOR - in;
		if (n > size - done) n = size - done;
		memcpy(buf + done, cd.ring + (size_t)(lba % CD_CAP) * RAW_SECTOR + in, n);
		done += n;
		in += n;
		if (in == RAW_SECTOR) { in = 0; lba++; }
	}
	pthread_mutex_unlock(&cd.lock);
	cd.pos += done;
	return done ? done : AVERROR_EOF;
}

static int64_t cd_io_seek(void *opaque, int64_t offset, int whence)
{
	(void)opaque;
	int64_t total = CD_HDR + (int64_t)(cd.end - cd.start) * RAW_SECTOR;
	if (whence == AVSEEK_SIZE) return total;
	whence &= ~AVSEEK_FORCE;
	if (whence == SEEK_CUR) offset += cd.pos;
	else if (whence == SEEK_END) offset += total;
	else if (whence != SEEK_SET) return -1;
	if (offset < 0) return -1;
	cd.pos = offset < total ? offset : total;
	return cd.pos;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void cd_make_header(void)
{
	uint32_t data = (uint32_t)(cd.end - cd.start) * RAW_SECTOR;
	uint8_t *h = cd.hdr;
	memcpy(h, "RIFF", 4);
	put32(h + 4, 36 + data);
	memcpy(h + 8, "WAVEfmt ", 8);
	put32(h + 16, 16);
	h[20] = 1; h[21] = 0;  // PCM
	h[22] = 2; h[23] = 0;  // stereo
	put32(h + 24, CD_RATE);
	put32(h + 28, CD_RATE * 4);
	h[32] = 4; h[33] = 0;
	h[34] = 16; h[35] = 0;
	memcpy(h + 36, "data", 4);
	put32(h + 40, data);
}

// ---------------------------------------------------------------------------
// Names of the tracks
// ---------------------------------------------------------------------------

static void cd_latin1_to_utf8(char *dst, size_t len, const uint8_t *src, size_t n)
{
	size_t o = 0;
	for (size_t i = 0; i < n && src[i] && o + 3 < len; i++)
	{
		uint8_t c = src[i];
		if (c < 0x20) continue;
		if (c < 0x80) dst[o++] = (char)c;
		else
		{
			dst[o++] = (char)(0xC0 | (c >> 6));
			dst[o++] = (char)(0x80 | (c & 63));
		}
	}
	dst[o] = 0;
}

// CD-Text (READ TOC format 5): titles (pack 0x80) and performers (0x81) of
// the first language block, when it is in ISO 8859-1 / ASCII.
static void cd_read_text(void)
{
	if (cd_fd < 0) return;
	static uint8_t resp[4 + 18 * 256];
	uint8_t cdb[10] = { 0x43, 0x00, 0x05 };
	uint8_t sense[32];
	cdb[7] = sizeof(resp) >> 8;
	cdb[8] = sizeof(resp) & 0xFF;
	sg_io_hdr_t io;
	memset(&io, 0, sizeof(io));
	memset(resp, 0, sizeof(resp));
	io.interface_id = 'S';
	io.cmd_len = sizeof(cdb);
	io.cmdp = cdb;
	io.dxfer_direction = SG_DXFER_FROM_DEV;
	io.dxferp = resp;
	io.dxfer_len = sizeof(resp);
	io.sbp = sense;
	io.mx_sb_len = sizeof(sense);
	io.timeout = 5000;
	if (ioctl(cd_fd, SG_IO, &io) < 0 || (io.info & SG_INFO_OK_MASK) != SG_INFO_OK)
	{
		printf("mediacenter: CD-Text non presente\n");
		return;
	}
	int len = ((resp[0] << 8) | resp[1]) - 2;
	if (len > (int)sizeof(resp) - 4) len = sizeof(resp) - 4;
	int packs = len / 18, found = 0;
	for (int kind = 0; kind < 2; kind++)
	{
		uint8_t type = kind ? 0x81 : 0x80;
		uint8_t str[160];
		int sl = 0, track = -1;
		for (int i = 0; i < packs; i++)
		{
			const uint8_t *p = resp + 4 + i * 18;
			if (p[0] != type || ((p[3] >> 4) & 7) != 0 || (p[3] & 0x80)) continue; // block 0, single byte
			if (track < 0) track = p[1] & 0x7F;
			for (int k = 4; k < 16; k++)
			{
				if (p[k])
				{
					if (sl < (int)sizeof(str) - 1) str[sl++] = p[k];
					continue;
				}
				str[sl] = 0;
				if (track >= 0 && track <= MAX_TRACKS && sl)
				{
					char *d = track ? (kind ? cd.performer[track] : cd.title[track]) : (kind ? cd.artist : cd.album);
					size_t dl = track ? sizeof(cd.title[0]) : sizeof(cd.album);
					if (sl == 1 && str[0] == '\t' && track > 1)
						snprintf(d, dl, "%s", kind ? cd.performer[track - 1] : cd.title[track - 1]);
					else cd_latin1_to_utf8(d, dl, str, sl);
					found++;
				}
				sl = 0;
				track++;
			}
		}
	}
	printf("mediacenter: CD-Text %s%s%s\n", found ? "presente" : "vuoto", cd.album[0] ? ": " : "", cd.album);
}

// TITLE and PERFORMER lines of a .cue: before the first TRACK they are the
// album's, after it the track's.
static void cd_read_cue_text(const char *cue)
{
	FILE *f = fopen(cue, "r");
	if (!f) return;
	char line[512], val[256];
	int t = 0, n;
	char mode[32];
	while (fgets(line, sizeof(line), f))
	{
		if (sscanf(line, " TRACK %d %31s", &n, mode) == 2 && n >= 1 && n <= MAX_TRACKS) t = n;
		else if (sscanf(line, " TITLE \"%255[^\"]\"", val) == 1)
			snprintf(t ? cd.title[t] : cd.album, sizeof(cd.album), "%s", val);
		else if (sscanf(line, " PERFORMER \"%255[^\"]\"", val) == 1)
			snprintf(t ? cd.performer[t] : cd.artist, sizeof(cd.artist), "%s", val);
	}
	fclose(f);
}

// ---------------------------------------------------------------------------
// Chapters and the screen
// ---------------------------------------------------------------------------

static int cd_track_at(double t)
{
	int lba = cd.start + (int)((t + 0.25) * 75);
	int k = cd.first;
	for (int i = cd.first; i <= cd.last; i++) if (tracks[i].start <= lba) k = i;
	return k;
}

static double cd_track_time(int t) { return (tracks[t].start - cd.start) / 75.0; }

static int cd_track_end(int t) { return t < cd.last ? tracks[t + 1].start : cd.end; }

// Called before each picture of the audio screen: the time and duration of
// the track, and its name when the track changes.
static void cd_screen(double *time, double *dur)
{
	int t = cd_track_at(*time);
	if (t != cd.shown)
	{
		cd.shown = t;
		char line[300];
		aui_track = t - cd.first + 1;
		aui_tracks = cd.last - cd.first + 1;
		if (cd.title[t][0]) snprintf(line, sizeof(line), "%s", cd.title[t]);
		else snprintf(line, sizeof(line), "Traccia %d", aui_track);
		aui_latin1(aui.title, sizeof(aui.title), line);
		const char *who = cd.performer[t][0] ? cd.performer[t] : cd.artist;
		aui_latin1(aui.file, sizeof(aui.file), who[0] ? who : "CD audio");
		snprintf(line, sizeof(line), "CD audio%s%s", cd.album[0] ? ": " : "", cd.album);
		aui_latin1(aui.folder, sizeof(aui.folder), line);
		snprintf(aui.fmt, sizeof(aui.fmt), "CD  44,1 kHz  16 bit  Stereo");
		aui.bg_state = -1; // the still part is drawn again
		printf("mediacenter: CD traccia %d di %d%s%s\n", aui_track, aui_tracks, cd.title[t][0] ? ": " : "", cd.title[t]);
	}
	double s = cd_track_time(t);
	*time -= s;
	if (*time < 0) *time = 0;
	*dur = (cd_track_end(t) - tracks[t].start) / 75.0;
}

// ---------------------------------------------------------------------------
// Playing
// ---------------------------------------------------------------------------

// Plays the audio tracks of the disc (cd_fd open, tracks read) or of an
// image (img_fd open). cue: the image's .cue for the names, or NULL.
static int cdda_play(const char *cue)
{
	// the first run of audio tracks (a CD Extra has its data track after it)
	int first = 0, last = 0;
	for (int t = track_first; t <= track_last; t++)
	{
		if (tracks[t].data) { if (first) break; continue; }
		if (!first) first = t;
		last = t;
	}
	if (!first) return -1;
	cd.first = first;
	cd.last = last;
	cd.start = tracks[first].start;
	cd.end = tracks[last].end;
	// before a data session (CD Extra): lead-out, lead-in and pre-gap of the
	// second session (11400 sectors) are not sound
	if (last < track_last && tracks[last + 1].data && cd.end - 11400 > tracks[last].start) cd.end -= 11400;
	if (cd.end <= cd.start) return -1;

	memset(cd.album, 0, sizeof(cd.album));
	memset(cd.artist, 0, sizeof(cd.artist));
	memset(cd.title, 0, sizeof(cd.title));
	memset(cd.performer, 0, sizeof(cd.performer));
	if (cue) cd_read_cue_text(cue);
	else cd_read_text();

	int speed = 8;
	const char *e = getenv("MC_CD_SPEED");
	if (e && atoi(e) > 0) speed = atoi(e);
	if (cd_fd >= 0 && ioctl(cd_fd, CDROM_SELECT_SPEED, speed)) printf("mediacenter: velocità del lettore non impostata\n");

	cd.ring = (uint8_t *)malloc((size_t)CD_CAP * RAW_SECTOR);
	const int io_size = 16 * RAW_SECTOR;
	uint8_t *io_buf = (uint8_t *)av_malloc(io_size);
	if (!cd.ring || !io_buf)
	{
		free(cd.ring);
		cd.ring = NULL;
		av_free(io_buf);
		return -1;
	}
	cd_make_header();
	cd.pos = 0;
	cd.lo = cd.hi = cd.cur = cd.start;
	cd.bad = cd.reread = 0;
	cd.shown = -1;
	cd.running = 1;
	if (pthread_create(&cd.thread, NULL, cd_reader, NULL))
	{
		cd.running = 0;
		free(cd.ring);
		cd.ring = NULL;
		av_free(io_buf);
		return -1;
	}
	media_io = avio_alloc_context(io_buf, io_size, 0, NULL, cd_io_read, NULL, cd_io_seek);
	printf("mediacenter: CD audio, tracce %d-%d, %d:%02d, settori %d-%d, velocità %dx\n", first, last,
		(cd.end - cd.start) / 75 / 60, (cd.end - cd.start) / 75 % 60, cd.start, cd.end, speed);

	int r = END_DONE;
	cdda_on = 1;
	if (media_io && !media_open("CD audio"))
	{
		snprintf(mc.title, sizeof(mc.title), "%s", cd.album[0] ? cd.album : "CD audio");
		native_fb = 0;
		r = media_play(0);
		if (!(mc.audio_only && fb_menu)) fb_native(0, 0, 0);
		media_close();
	}
	else fprintf(stderr, "mediacenter: CD audio non leggibile\n");
	cdda_on = 0;

	pthread_mutex_lock(&cd.lock);
	cd.running = 0;
	pthread_cond_broadcast(&cd.cond);
	pthread_mutex_unlock(&cd.lock);
	pthread_join(cd.thread, NULL);
	printf("mediacenter: CD audio: %d settori riletti, %d illeggibili (silenzio)\n", cd.reread, cd.bad);
	if (media_io)
	{
		av_freep(&media_io->buffer);
		avio_context_free(&media_io);
	}
	else av_free(io_buf);
	free(cd.ring);
	cd.ring = NULL;
	if (cd_fd >= 0) ioctl(cd_fd, CDROM_SELECT_SPEED, 0); // the drive's own speed again
	aui_track = aui_tracks = 0;
	return r;
}

static int cd_chapters(void) { return cd.last - cd.first + 1; }
static int cd_chapter_index(double t) { return cd_track_at(t) - cd.first; }
static double cd_chapter_start(int c) { return cd_track_time(cd.first + c); }
