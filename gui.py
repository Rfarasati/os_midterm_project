import streamlit as st
import requests
import time
import os
import json
from datetime import datetime

st.set_page_config(
    page_title="IPFS-like Storage System",
    page_icon="📦",
    layout="wide"
)

# API endpoints
API_BASE = "http://127.0.0.1:9000"

st.title("📦 Content-Addressed Storage System")
st.markdown("*IPFS-like distributed storage with deduplication*")

# Sidebar - System Status
st.sidebar.header("📊 System Status")

def get_system_stats():
    """Get current system statistics"""
    blocks_count = 0
    manifests_count = 0

    try:
        # Count blocks
        for root, dirs, files in os.walk("blocks"):
            blocks_count += len(files)

        # Count manifests
        if os.path.exists("manifests"):
            manifests_count = len([f for f in os.listdir("manifests") if f.endswith('.json')])
    except:
        pass

    return blocks_count, manifests_count

blocks, manifests = get_system_stats()
st.sidebar.metric("Total Blocks", blocks)
st.sidebar.metric("Total Files", manifests)
st.sidebar.markdown("---")

# Main tabs
tab1, tab2, tab3, tab4 = st.tabs(["📤 Upload", "📥 Download", "📋 Files", "🖥️ Logs"])

# ============================================================================
# TAB 1: UPLOAD
# ============================================================================
with tab1:
    st.header("Upload Files")
    st.markdown("Upload files to the content-addressed storage system. Files are automatically chunked and deduplicated.")

    uploaded_files = st.file_uploader(
        "Choose files to upload",
        accept_multiple_files=True,
        help="Files will be split into 256KB chunks and stored with content addressing"
    )

    if st.button("🚀 Upload All", type="primary", disabled=not uploaded_files):
        progress_bar = st.progress(0)
        status_text = st.empty()
        results_container = st.container()

        for idx, uploaded_file in enumerate(uploaded_files):
            status_text.text(f"Uploading {uploaded_file.name}...")

            try:
                # Read file content
                file_content = uploaded_file.read()

                # Upload to server
                headers = {
                    'X-Filename': uploaded_file.name,
                    'Content-Length': str(len(file_content))
                }

                start_time = time.time()
                response = requests.post(
                    f"{API_BASE}/upload",
                    headers=headers,
                    data=file_content,
                    timeout=30
                )
                elapsed = time.time() - start_time

                if response.status_code == 200:
                    result = response.json()
                    cid = result['cid']

                    with results_container:
                        st.success(f"✅ **{uploaded_file.name}**")
                        col1, col2, col3 = st.columns([2, 1, 1])
                        with col1:
                            st.code(cid, language=None)
                        with col2:
                            st.text(f"Size: {len(file_content):,} bytes")
                        with col3:
                            st.text(f"Time: {elapsed:.2f}s")
                else:
                    with results_container:
                        st.error(f"❌ Failed: {uploaded_file.name} - Status {response.status_code}")

            except Exception as e:
                with results_container:
                    st.error(f"❌ Error uploading {uploaded_file.name}: {str(e)}")

            progress_bar.progress((idx + 1) / len(uploaded_files))

        status_text.text("✅ All uploads complete!")
        time.sleep(1)
        st.rerun()

# ============================================================================
# TAB 2: DOWNLOAD
# ============================================================================
with tab2:
    st.header("Download Files")
    st.markdown("Enter a CID to download the corresponding file.")

    col1, col2 = st.columns([3, 1])

    with col1:
        cid_input = st.text_input(
            "Content Identifier (CID)",
            placeholder="fa2nv5xm2cr6ojkxnwpwtqyqhamwm4doinxfkzoo3akyjbmzgruq",
            help="Paste the CID you received when uploading"
        )

    with col2:
        st.write("")  # Spacing
        st.write("")  # Spacing
        download_button = st.button("📥 Download", type="primary")

    if download_button and cid_input:
        try:
            with st.spinner(f"Downloading CID: {cid_input[:20]}..."):
                response = requests.get(
                    f"{API_BASE}/download",
                    params={'cid': cid_input},
                    timeout=30
                )

            if response.status_code == 200:
                # Get filename from manifest if possible
                filename = f"downloaded_{cid_input[:16]}.bin"

                try:
                    manifest_path = f"manifests/{cid_input}.json"
                    if os.path.exists(manifest_path):
                        with open(manifest_path, 'r') as f:
                            manifest = json.load(f)
                            filename = manifest.get('filename', filename)
                except:
                    pass

                st.success(f"✅ File retrieved successfully!")

                # Provide download button
                st.download_button(
                    label=f"💾 Save as {filename}",
                    data=response.content,
                    file_name=filename,
                    mime="application/octet-stream"
                )

                # Show file info
                col1, col2 = st.columns(2)
                with col1:
                    st.metric("File Size", f"{len(response.content):,} bytes")
                with col2:
                    chunks = (len(response.content) + 262143) // 262144
                    st.metric("Chunks", chunks)

            else:
                st.error(f"❌ Download failed: Status {response.status_code}")
                st.text("Possible reasons:")
                st.text("• CID not found")
                st.text("• Invalid CID format")
                st.text("• Server error")

        except Exception as e:
            st.error(f"❌ Error: {str(e)}")

# ============================================================================
# TAB 3: FILES LIST
# ============================================================================
with tab3:
    st.header("Stored Files")
    st.markdown("All files currently stored in the system")

    if st.button("🔄 Refresh"):
        st.rerun()

    try:
        if os.path.exists("manifests"):
            manifests_list = [f for f in os.listdir("manifests") if f.endswith('.json')]

            if manifests_list:
                st.subheader(f"📁 {len(manifests_list)} files stored")

                for manifest_file in sorted(manifests_list, reverse=True):
                    cid = manifest_file.replace('.json', '')

                    try:
                        with open(f"manifests/{manifest_file}", 'r') as f:
                            manifest = json.load(f)

                        with st.expander(f"📄 {manifest.get('filename', 'Unknown')}"):
                            col1, col2, col3 = st.columns(3)

                            with col1:
                                st.text("CID:")
                                st.code(cid, language=None)

                            with col2:
                                st.metric("Size", f"{manifest.get('total_size', 0):,} bytes")
                                st.metric("Chunks", len(manifest.get('chunks', [])))

                            with col3:
                                # Download button
                                if st.button(f"📥 Download", key=f"dl_{cid}"):
                                    try:
                                        response = requests.get(
                                            f"{API_BASE}/download",
                                            params={'cid': cid}
                                        )
                                        if response.status_code == 200:
                                            st.download_button(
                                                label="💾 Save File",
                                                data=response.content,
                                                file_name=manifest.get('filename', 'file.bin'),
                                                key=f"save_{cid}"
                                            )
                                    except:
                                        st.error("Download failed")

                            # Show chunk details
                            if st.checkbox("Show chunk details", key=f"details_{cid}"):
                                st.json(manifest)
                    except:
                        st.error(f"Error reading manifest: {manifest_file}")
            else:
                st.info("📭 No files uploaded yet")
        else:
            st.info("📭 No manifests directory found")
    except Exception as e:
        st.error(f"Error: {str(e)}")

# ============================================================================
# TAB 4: LOGS
# ============================================================================
with tab4:
    st.header("System Logs")
    st.markdown("Real-time view of storage operations")

    col1, col2 = st.columns([3, 1])
    with col2:
        auto_refresh = st.checkbox("Auto-refresh (5s)", value=False)

    # Show block storage structure
    st.subheader("📦 Block Storage Structure")

    try:
        if os.path.exists("blocks"):
            block_tree = {}
            for root, dirs, files in os.walk("blocks"):
                if files:
                    relative_path = root.replace("blocks/", "")
                    block_tree[relative_path] = len(files)

            if block_tree:
                for path, count in sorted(block_tree.items())[:20]:  # Show first 20
                    st.text(f"blocks/{path}/ → {count} blocks")

                if len(block_tree) > 20:
                    st.text(f"... and {len(block_tree) - 20} more directories")
            else:
                st.info("No blocks stored yet")
        else:
            st.info("Blocks directory not found")
    except Exception as e:
        st.error(f"Error reading blocks: {str(e)}")

    st.markdown("---")

    # Show recent manifests
    st.subheader("📋 Recent Uploads")
    try:
        if os.path.exists("manifests"):
            manifests = []
            for f in os.listdir("manifests"):
                if f.endswith('.json'):
                    path = f"manifests/{f}"
                    mtime = os.path.getmtime(path)
                    manifests.append((f, mtime))

            manifests.sort(key=lambda x: x[1], reverse=True)

            for manifest_file, mtime in manifests[:10]:  # Last 10
                cid = manifest_file.replace('.json', '')
                dt = datetime.fromtimestamp(mtime)

                try:
                    with open(f"manifests/{manifest_file}", 'r') as f:
                        manifest = json.load(f)

                    st.text(f"[{dt.strftime('%H:%M:%S')}] {manifest.get('filename', 'Unknown')} → {cid[:40]}...")
                except:
                    pass
    except:
        pass

    if auto_refresh:
        time.sleep(5)
        st.rerun()

# Footer
st.markdown("---")
st.markdown(
    """
    <div style='text-align: center; color: gray;'>
    <p>Content-Addressed Storage System | OS Midterm Project</p>
    <p>Built with ❤️ using Python, C, and Streamlit</p>
    </div>
    """,
    unsafe_allow_html=True
)