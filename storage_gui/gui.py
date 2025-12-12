import streamlit as st
import requests
import time
from datetime import datetime

st.set_page_config(
    page_title="IPFS-like Storage System",
    page_icon="📦",
    layout="wide"
)

# API endpoint - connects to WSL
API_BASE = "http://127.0.0.1:9000"

st.title("📦 Content-Addressed Storage System")
st.markdown("*IPFS-like distributed storage*")
st.markdown("🔗 Connected to: `WSL Ubuntu (127.0.0.1:9000)`")

# Check connection
try:
    response = requests.get(f"{API_BASE}/", timeout=1)
    connection_status = "🟢 Connected"
except:
    connection_status = "🔴 Disconnected"

st.sidebar.header("📊 System Status")
st.sidebar.markdown(f"**Status:** {connection_status}")
st.sidebar.markdown("---")

# Main tabs
tab1, tab2, tab3 = st.tabs(["📤 Upload", "📥 Download", "ℹ️ About"])

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

    col1, col2 = st.columns([3, 1])

    with col2:
        upload_button = st.button("🚀 Upload All", type="primary", disabled=not uploaded_files)

    if upload_button and uploaded_files:
        progress_bar = st.progress(0)
        status_text = st.empty()
        results_container = st.container()

        for idx, uploaded_file in enumerate(uploaded_files):
            status_text.text(f"Uploading {uploaded_file.name}... ({idx+1}/{len(uploaded_files)})")

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
                            st.text(f"📦 Size: {len(file_content):,} bytes")
                        with col3:
                            st.text(f"⏱️ Time: {elapsed:.2f}s")
                else:
                    with results_container:
                        st.error(f"❌ Failed: {uploaded_file.name} - Status {response.status_code}")

            except requests.exceptions.ConnectionError:
                with results_container:
                    st.error(f"❌ Connection Error: Make sure C engine and gateway are running in WSL!")
                break
            except Exception as e:
                with results_container:
                    st.error(f"❌ Error uploading {uploaded_file.name}: {str(e)}")

            progress_bar.progress((idx + 1) / len(uploaded_files))

        status_text.text("✅ All uploads complete!")
        st.balloons()

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
                filename = f"downloaded_{cid_input[:16]}.bin"

                st.success(f"✅ File retrieved successfully!")

                # Provide download button
                st.download_button(
                    label=f"💾 Save as {filename}",
                    data=response.content,
                    file_name=filename,
                    mime="application/octet-stream",
                    type="primary"
                )

                # Show file info
                col1, col2 = st.columns(2)
                with col1:
                    st.metric("📦 File Size", f"{len(response.content):,} bytes")
                with col2:
                    chunks = (len(response.content) + 262143) // 262144
                    st.metric("🧩 Chunks", chunks)

                # Show hex preview for binary files
                if len(response.content) > 0:
                    with st.expander("🔍 File Preview (first 256 bytes)"):
                        preview = response.content[:256]
                        hex_str = preview.hex()
                        formatted_hex = ' '.join(hex_str[i:i+2] for i in range(0, len(hex_str), 2))
                        st.code(formatted_hex, language=None)

                        # Try to show as text if possible
                        try:
                            text_preview = preview.decode('utf-8')
                            st.text_area("Text Preview:", text_preview, height=100)
                        except:
                            st.info("(Binary file - cannot display as text)")

            else:
                st.error(f"❌ Download failed: Status {response.status_code}")
                st.markdown("""
                **Possible reasons:**
                - CID not found
                - Invalid CID format
                - Server error
                
                Make sure:
                1. The CID is correct
                2. The file was uploaded successfully
                3. C engine is running in WSL
                """)

        except requests.exceptions.ConnectionError:
            st.error("❌ Connection Error: Cannot reach server!")
            st.markdown("""
            **Troubleshooting:**
            1. Check if C engine is running: `./c_engine /tmp/cengine.sock`
            2. Check if gateway is running: `python3 main.py`
            3. Verify WSL is accessible from Windows
            """)
        except Exception as e:
            st.error(f"❌ Error: {str(e)}")

# ============================================================================
# TAB 3: ABOUT
# ============================================================================
with tab3:
    st.header("ℹ️ About This System")

    col1, col2 = st.columns(2)

    with col1:
        st.subheader("🏗️ Architecture")

        st.subheader("📋 Features")
        st.markdown("""
        - ✅ Content-addressed storage
        - ✅ Automatic chunking (256KB)
        - ✅ Block deduplication
        - ✅ Parallel processing (8 threads)
        - ✅ Merkle-DAG structure
        - ✅ SHA-256 hashing
        - ✅ Concurrent operations
        """)

    with col2:
        st.subheader("🔧 How It Works")
        st.markdown("""
        **Upload Flow:**
        1. Select file in GUI
        2. File sent to gateway via HTTP
        3. Gateway splits into 256KB chunks
        4. C engine hashes each chunk (SHA-256)
        5. Chunks stored in blocks directory
        6. Manifest generated with chunk list
        7. CID = hash(manifest)
        8. CID returned to GUI
        
        **Download Flow:**
        1. Enter CID in GUI
        2. Gateway requests file from engine
        3. Engine loads manifest
        4. Chunks verified in parallel (8 threads)
        5. Chunks streamed in order
        6. File reconstructed
        7. File available for download
        """)

        st.subheader("🎓 Project Info")
        st.markdown("""
        **Operating Systems - Midterm Project**
        - Content-Addressed Storage System
        - IPFS-like implementation
        - Reza Ferasti & Negar Bahrampoor
        """)

# Footer
st.markdown("---")
st.markdown(
    """
    <div style='text-align: center; color: gray;'>
    <p><strong>Content-Addressed Storage System</strong> | OS Midterm Project | Reza Ferasti & Negar Bahrampoor</p>
    </div>
    """,
    unsafe_allow_html=True
)

# Debug info in sidebar (collapsible)
with st.sidebar.expander("🔧 Debug Info"):
    st.text(f"API Base: {API_BASE}")
    st.text(f"Connection: {connection_status}")

    if st.button("Test Connection"):
        try:
            response = requests.get(f"{API_BASE}/", timeout=1)
            st.success("✅ Connection successful!")
        except requests.exceptions.ConnectionError:
            st.error("❌ Cannot connect to WSL")
            st.markdown("""
            **To fix:**
```bash
            # In WSL terminal:
            cd ~/os_project
            ./c_engine /tmp/cengine.sock
            
            # In another WSL terminal:
            python3 main.py
```
            """)
        except Exception as e:
            st.error(f"❌ Error: {e}")