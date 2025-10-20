#include "proxy-student.h"
#include "gfserver.h"



#define MAX_REQUEST_N 512
#define BUFSIZE (6426)

// Callback to download and transfer file trunk by trunk
size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
	ssize_t write_len = gfs_send(userdata, ptr, size*nmemb);
	// Debug: fprintf(stdout, "Receive: %ld bytes, Transfer: %ld bytes.\n", write_len, size*nmemb);
	return write_len;
}

ssize_t handle_with_curl(gfcontext_t *ctx, const char *path, void* arg){
	const char *server = (const char *)arg;

	// Manually concatenate server_url and path
	size_t len = strlen(server) + strlen(path) + 1;
	char complete_url[len];
	snprintf(complete_url, sizeof(complete_url), "%s%s", server, path);

	// Parse the URL
	CURLU *h = curl_url();
	if (!h) {
		fprintf(stderr, "Failed to create CURLU handle.\n");
		return SERVER_FAILURE;
	}

	CURLUcode uc = curl_url_set(h, CURLUPART_URL, complete_url, CURLU_DEFAULT_SCHEME);
	if (uc != CURLUE_OK) {
		fprintf(stderr, "Failed to set URL: %s with error code %d.\n", complete_url, uc);
		curl_url_cleanup(h);
		return SERVER_FAILURE;
	}

	curl_url_cleanup(h);
	// Debug: fprintf(stdout, "URL set to: %s\n", complete_url);

	// Init the curl handle
	CURL *curl = curl_easy_init();
	if(!curl) {
		fprintf(stderr, "Curl initialization failed.\n");
		curl_easy_cleanup(curl);
		return SERVER_FAILURE;
	}
	curl_easy_setopt(curl, CURLOPT_URL, complete_url);

	// Check the existence of the file
	curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L); // make 404/500 cause CURLE_HTTP_RETURNED_ERROR

	CURLcode ex = curl_easy_perform(curl);
	if (ex == CURLE_HTTP_RETURNED_ERROR) {
		// HTTP error (e.g., 404 Not Found)
		curl_easy_cleanup(curl);
		fprintf(stderr, "HTTP server error with code %d.\n", ex);
		return gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
	}
	if (ex != CURLE_OK) {
		// Network errors
		fprintf(stderr, "Network error with code %d.\n", ex);
		curl_easy_cleanup(curl);
		return SERVER_FAILURE;
	}

	// Retrieve the file size and send header
	size_t file_len;
	CURLcode getlen = curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &file_len);
	if (getlen != CURLE_OK) {
		fprintf(stderr, "Error occurs trying to retrieve file length with code %d.\n", getlen);
		curl_easy_cleanup(curl);
		return SERVER_FAILURE;
	}
	// Debug: fprintf(stdout, "File length: %lu bytes.\n", file_len);
	gfs_sendheader(ctx, GF_OK, file_len);

	// Register the callback, download and transfer the file
	curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx);
	CURLcode download = curl_easy_perform(curl);
	if (download != CURLE_OK) {
		fprintf(stderr, "Error occurs trying to download file with code %d.\n", download);
		curl_easy_cleanup(curl);
		return SERVER_FAILURE;
	}
	curl_easy_cleanup(curl);
	return (ssize_t)file_len;
}

/*
 * We provide a dummy version of handle_with_file that invokes handle_with_curl as a convenience for linking!
 */
ssize_t handle_with_file(gfcontext_t *ctx, const char *path, void* arg){
	return handle_with_curl(ctx, path, arg);
}	
