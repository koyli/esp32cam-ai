FROM python:3.11-slim
RUN pip install flask
RUN useradd -m appuser
WORKDIR /app
COPY upload_server.py .
USER appuser
CMD ["python3", "upload_server.py"]
